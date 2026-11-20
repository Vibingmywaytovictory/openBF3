import sys

from look_shot_colours import read_png

CHANGED = 32
MIN_HALF_PIXELS = 400


def pixel(rows, channels, x, y):
    at = x * channels
    return rows[y][at:at + 3]


def centre(quad):
    (x1, y1), (x2, y2), (x3, y3), (x4, y4) = quad
    d = (x1 - x3) * (y2 - y4) - (y1 - y3) * (x2 - x4)
    if abs(d) < 1e-9:
        return None
    t = ((x1 - x2) * (y2 - y4) - (y1 - y2) * (x2 - x4)) / d
    return x1 + t * (x3 - x1), y1 + t * (y3 - y1)


def inside(quad, x, y):
    sign = 0
    for i in range(4):
        ax, ay = quad[i]
        bx, by = quad[(i + 1) % 4]
        c = (bx - ax) * (y - ay) - (by - ay) * (x - ax)
        if c != 0:
            s = 1 if c > 0 else -1
            if sign != 0 and s != sign:
                return False
            sign = s
    return True


def differs(rows_a, channels_a, rows_b, channels_b, x, y):
    a = pixel(rows_a, channels_a, x, y)
    b = pixel(rows_b, channels_b, x, y)
    return max(abs(a[k] - b[k]) for k in range(3)) > CHANGED


def halves(on, ref, off, quad):
    width, height, channels_on, rows_on = on
    _, _, channels_ref, rows_ref = ref
    _, _, channels_off, rows_off = off
    mid = centre(quad)
    if mid is None:
        return None
    x0 = max(0, int(min(p[0] for p in quad)))
    x1 = min(width - 1, int(max(p[0] for p in quad)) + 1)
    y0 = max(0, int(min(p[1] for p in quad)))
    y1 = min(height - 1, int(max(p[1] for p in quad)) + 1)
    counts = {'near': [0, 0], 'far': [0, 0]}
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if not inside(quad, x + 0.5, y + 0.5):
                continue
            if not differs(rows_ref, channels_ref, rows_off, channels_off, x, y):
                continue
            half = counts['near' if y + 0.5 >= mid[1] else 'far']
            half[0] += 1
            if differs(rows_on, channels_on, rows_ref, channels_ref, x, y):
                half[1] += 1
    return counts


def main(argv):
    if len(argv) != 6:
        print('usage: surface_decal_shots.py <max-hidden-share> <on-dir> <lifted-dir> <off-dir> <points-file>')
        return 2
    limit = float(argv[1])
    failed = False
    with open(argv[5]) as f:
        views = [line.rstrip('\n').split('|') for line in f if '|' in line]
    if not views:
        print('surface decal shots: no view with projected points')
        return 1
    for name, points in views:
        on = read_png(argv[2] + '/' + name)
        ref = read_png(argv[3] + '/' + name)
        off = read_png(argv[4] + '/' + name)
        if on[:2] != ref[:2] or on[:2] != off[:2]:
            print('surface decal shot %s: the shots differ in size' % name)
            failed = True
            continue
        corners = points.split()
        measured = 0
        for q in range(0, len(corners) - 3, 4):
            group = corners[q:q + 4]
            if 'behind' in group:
                continue
            quad = [tuple(float(v) for v in c.split(',')) for c in group]
            counts = halves(on, ref, off, quad)
            if counts is None or min(counts['near'][0], counts['far'][0]) < MIN_HALF_PIXELS:
                continue
            measured += 1
            for side in ('near', 'far'):
                drawn, hidden = counts[side]
                share = hidden / float(drawn)
                over = share > limit
                failed = failed or over
                print('surface decal shot %s grating %d %s half: %d of the %d pixels the grating draws lifted 1 %% toward the camera '
                      'are hidden, share %.3f (limit %.3f) %s' % (name, q // 4, side, hidden, drawn, share, limit,
                                                               'FAIL' if over else 'ok'))
        if measured == 0:
            print('surface decal shot %s: no grating on screen' % name)
            failed = True
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

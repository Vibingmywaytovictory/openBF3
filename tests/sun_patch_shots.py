import sys

from look_shot_colours import read_png

VIEWS = ('toward', 'away', 'side_a', 'side_b')


def window_luma(path, centre, half):
    width, height, channels, rows = read_png(path)
    cx, cy = centre
    x0, x1 = max(0, cx - half), min(width, cx + half)
    y0, y1 = max(0, cy - half), min(height, cy + half)
    total = 0
    count = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            at = x * channels
            total += row[at] * 299 + row[at + 1] * 587 + row[at + 2] * 114
            count += 1
    return total / (count * 1000.0) if count else None


def main(argv):
    if len(argv) != 4 + len(VIEWS):
        print('usage: sun_patch_shots.py <shots-dir> <prefix> <half-window> ' +
              ' '.join(v + '=<x>,<y>' for v in VIEWS))
        return 2
    half = int(argv[3])
    centres = {}
    for item in argv[4:]:
        name, _, xy = item.partition('=')
        x, _, y = xy.partition(',')
        centres[name] = (int(round(float(x))), int(round(float(y))))
    means = {}
    for name in VIEWS:
        if name not in centres:
            print('sun patch shots: no centre for ' + name)
            return 1
        means[name] = window_luma('%s/%s%s.png' % (argv[1], argv[2], name), centres[name], half)
        if means[name] is None:
            print('sun patch shots: the window of %s is off the shot' % name)
            return 1
    others = [means[name] for name in VIEWS[1:]]
    limit = max(others) + (max(others) - min(others))
    print('sun patch shots: %s window %dx%d luma toward %.2f, away %.2f, sides %.2f %.2f; limit %.2f' %
          (argv[2], 2 * half, 2 * half, means['toward'], means['away'], means['side_a'], means['side_b'], limit))
    return 0 if means['toward'] <= limit else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))

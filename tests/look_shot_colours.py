import struct
import sys
import zlib

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
MAGENTA_MARGIN = 48
MAGENTA_FLOOR = 96


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(path + ': not a PNG')
    at = len(PNG_SIGNATURE)
    width = height = depth = colour = None
    idat = bytearray()
    while at + 8 <= len(data):
        length, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or colour not in (2, 6) or interlace != 0:
                raise ValueError(path + ': only 8-bit RGB or RGBA, not interlaced')
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    if width is None:
        raise ValueError(path + ': no IHDR')
    channels = 3 if colour == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                line[x] = (line[x] + paeth(left, up, corner)) & 0xFF
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def magenta_share(path):
    width, height, channels, rows = read_png(path)
    magenta = 0
    for line in rows:
        for x in range(0, width * channels, channels):
            r, g, b = line[x], line[x + 1], line[x + 2]
            if r >= MAGENTA_FLOOR and b >= MAGENTA_FLOOR and r - g >= MAGENTA_MARGIN and b - g >= MAGENTA_MARGIN:
                magenta += 1
    return magenta / float(width * height)


def main(argv):
    if len(argv) < 3:
        print('usage: look_shot_colours.py <max-share> <png>...')
        return 2
    limit = float(argv[1])
    failed = False
    for path in argv[2:]:
        share = magenta_share(path)
        over = share > limit
        failed = failed or over
        print('look shot %s: magenta share %.5f (limit %.5f) %s' % (path, share, limit, 'FAIL' if over else 'ok'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

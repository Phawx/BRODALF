#!/usr/bin/env python3
"""Draws BRODALF's icon and writes gui/brodalf.ico (16, 32, 48 and 256 px).

    python3 tools/make_icon.py

Only the standard library: each size is drawn from shapes with 4x4
supersampling, stored as a 32-bit bitmap with alpha (and as PNG at 256 px,
as Windows expects there). The picture is a backup drive with a check mark
above it on a blue tile: the copy on the drive is good.
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'gui', 'brodalf.ico')
SIZES = (16, 32, 48, 256)

TILE = (32, 101, 170)
TILE_EDGE = (22, 72, 128)
DRIVE = (244, 247, 250)
SLOT = (150, 168, 186)
LED = (62, 196, 92)
CHECK = (255, 255, 255)


def rounded_rect(x, y, x0, y0, x1, y1, r):
    """Inside a rectangle with corners rounded by r (unit coordinates)."""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def near_segment(x, y, ax, ay, bx, by, w):
    """Within w of the segment from (ax, ay) to (bx, by)."""
    dx, dy = bx - ax, by - ay
    t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)
    t = min(max(t, 0.0), 1.0)
    px, py = ax + t * dx, ay + t * dy
    return (x - px) ** 2 + (y - py) ** 2 <= w * w


def colour_at(x, y, size):
    """The colour at a point of the unit square, or None outside the icon.
    The thinnest parts get thicker at small sizes so they still show."""
    small = size <= 16
    if not rounded_rect(x, y, 0.02, 0.02, 0.98, 0.98, 0.2):
        return None
    colour = TILE if rounded_rect(x, y, 0.06, 0.06, 0.94, 0.94, 0.17) else TILE_EDGE
    # The drive: a flat box across the lower part, with a slot and a light.
    if rounded_rect(x, y, 0.14, 0.58, 0.86, 0.86, 0.07):
        colour = DRIVE
        if rounded_rect(x, y, 0.22, 0.70, 0.58, 0.75 if not small else 0.77, 0.02):
            colour = SLOT
        if (x - 0.74) ** 2 + (y - 0.725) ** 2 <= (0.055 if not small else 0.07) ** 2:
            colour = LED
    # The check mark above it.
    w = 0.075 if not small else 0.095
    if near_segment(x, y, 0.30, 0.31, 0.44, 0.45, w) or near_segment(x, y, 0.44, 0.45, 0.72, 0.15, w):
        colour = CHECK
    return colour


def draw(size):
    """RGBA rows, top first."""
    n = 4
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0
            for sy in range(n):
                for sx in range(n):
                    c = colour_at((px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size, size)
                    if c:
                        r += c[0]
                        g += c[1]
                        b += c[2]
                        a += 1
            if a:
                row += bytes((r // a, g // a, b // a, a * 255 // (n * n)))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def as_bmp(rows, size):
    """BITMAPINFOHEADER, the BGRA pixels bottom row first, then the AND
    mask (all clear: the alpha is what counts)."""
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for row in reversed(rows):
        for i in range(0, len(row), 4):
            r, g, b, a = row[i:i + 4]
            pixels += bytes((b, g, r, a))
    mask_row = ((size + 31) // 32) * 4
    return header + bytes(pixels) + bytes(mask_row * size)


def as_png(rows, size):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + row for row in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    images = []
    for size in SIZES:
        rows = draw(size)
        images.append((size, as_png(rows, size) if size >= 256 else as_bmp(rows, size)))
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        side = 0 if size >= 256 else size
        out += struct.pack('<BBBBHHII', side, side, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b''.join(data for _, data in images)
    with open(OUT, 'wb') as f:
        f.write(out)
    print('%s: %d bytes, %s px' % (os.path.relpath(OUT, ROOT), len(out), ', '.join(str(s) for s in SIZES)))


if __name__ == '__main__':
    main()

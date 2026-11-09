#!/usr/bin/env python3
# ppm2png.py in.ppm out.png — a binary PPM (P6) to PNG with the standard library only (zlib), half size.
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
parts = data.split(b"\n", 3)
assert parts[0] == b"P6", "not a binary PPM"
w, h = map(int, parts[1].split())
pix = parts[3]
# Half size: every other pixel of every other row (enough to read a menu, a quarter of the bytes).
ow, oh = w // 2, h // 2
rows = bytearray()
for y in range(oh):
    row = pix[(2 * y) * w * 3:(2 * y + 1) * w * 3]
    rows.append(0)
    rows += b"".join(row[x * 6:x * 6 + 3] for x in range(ow))


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)

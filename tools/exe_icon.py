#!/usr/bin/env python3
"""exe_icon.py <exe> <output.ico>: extracts the largest icon (RT_ICON) from a 32-bit PE's resources as a .ico.

Read from the user's copy of the game: the icon is never stored in the repository.
"""
import struct
import sys


def main(exe, out):
    raw = open(exe, "rb").read()
    pe = struct.unpack_from("<I", raw, 0x3C)[0]
    ns = struct.unpack_from("<H", raw, pe + 6)[0]
    opt = struct.unpack_from("<H", raw, pe + 20)[0]
    secs = [struct.unpack_from("<IIII", raw, pe + 24 + opt + i * 40 + 8) for i in range(ns)]  # vsize, va, rsize, rptr

    def off(rva):
        for _, va, rs, ro in secs:
            if va <= rva < va + rs:
                return rva - va + ro
        raise SystemExit("RVA outside sections: %x" % rva)

    base = off(struct.unpack_from("<I", raw, pe + 24 + 96 + 2 * 8)[0])   # data directory #2 = resources

    def entries(o):
        nn, ni = struct.unpack_from("<HH", raw, base + o + 12)
        for k in range(nn + ni):
            yield struct.unpack_from("<II", raw, base + o + 16 + 8 * k)

    icons = []
    for typ, d in entries(0):
        if typ != 3:   # RT_ICON
            continue
        for _, d2 in entries(d & 0x7FFFFFFF):
            for _, d3 in entries(d2 & 0x7FFFFFFF):
                rva, size = struct.unpack_from("<II", raw, base + d3)
                icons.append(raw[off(rva):off(rva) + size])
    if not icons:
        raise SystemExit("no icon in %s" % exe)
    data = max(icons, key=len)
    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(data), 22) + data
    open(out, "wb").write(ico)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

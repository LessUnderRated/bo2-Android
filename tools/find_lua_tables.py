#!/usr/bin/env python3
"""
find_lua_tables.py — recover Lua C-function registration tables from t6sp.exe.

Recipe (from docs/handoff/2026-07-28-session-report.md): scan .rdata and .data
for runs of (name_ptr, code_ptr) pairs — name_ptr pointing at a C string,
code_ptr pointing into executable code. Lua libraries (the 130 LUI builtins,
the Lua base/debug tables, and the still-uninstrumented Engine.* table) are
all registered from tables of exactly that shape.

Prints each run of >= MIN_RUN pairs with its VA, size, and the first names,
so the Engine table can be identified by what it contains.
"""
import struct
import sys
from collections import defaultdict

BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"
IMAGE_BASE = 0x400000
MIN_RUN = 3


def parse_sections(data: bytes):
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off:pe_off + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    secs = []
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
        chars = struct.unpack_from("<I", data, off + 36)[0]
        secs.append(dict(name=name, va=IMAGE_BASE + vaddr, vsize=vsize,
                         raw=raddr, rsize=rsize, chars=chars))
    return secs


def main():
    data = open(BIN, "rb").read()
    secs = parse_sections(data)
    code_secs = [s for s in secs if s["chars"] & 0x20000000]          # EXECUTE
    str_secs = [s for s in secs if s["chars"] & 0x40000000 and s["rsize"] > 0
                and s["name"] in (".rdata", ".data", ".rodata")]
    scan_secs = [s for s in secs if s["name"] in (".rdata", ".data") and s["rsize"] > 0]

    def in_code(va):
        return any(s["va"] <= va < s["va"] + s["vsize"] for s in code_secs)

    def read_cstr(va, maxlen=96):
        for s in str_secs:
            if s["va"] <= va < s["va"] + s["rsize"]:
                off = s["raw"] + (va - s["va"])
                end = data.find(b"\0", off, off + maxlen)
                if end < 0:
                    return None
                s_ = data[off:end]
                if len(s_) < 2:
                    return None
                if all(0x20 <= c < 0x7F for c in s_):
                    return s_.decode("ascii")
                return None
        return None

    # Gather (table_va, name, code_va) hits in both field orders.
    # ⚠️ A hit consumes TWO dwords: without skipping ahead, the overlapping
    # pair (code_of_prev, name_of_next) at +4 ALSO matches in reversed order
    # and breaks every run into length-1 fragments below MIN_RUN.
    hits = {}   # table_va -> (name, code_va)
    for s in scan_secs:
        base, raw, rsize = s["va"], s["raw"], s["rsize"]
        blob = data[raw:raw + rsize]
        n = rsize // 4
        dwords = struct.unpack_from("<%dI" % n, blob)
        i = 0
        while i < n - 1:
            a, b = dwords[i], dwords[i + 1]
            nm = read_cstr(a) if 0x400000 <= a < 0x4000000 else None
            if nm and in_code(b):
                hits[base + i * 4] = (nm, b)
                i += 2
                continue
            nm2 = read_cstr(b) if 0x400000 <= b < 0x4000000 else None
            if nm2 and in_code(a):
                hits[base + i * 4] = (nm2, a)
                i += 2
                continue
            i += 1

    # Group into runs of consecutive 8-byte slots.
    runs = []
    cur = []
    for va in sorted(hits):
        if cur and va != cur[-1] + 8:
            runs.append(cur)
            cur = []
        cur.append(va)
    runs.append(cur)

    big = [r for r in runs if len(r) >= MIN_RUN]
    big.sort(key=lambda r: -len(r))
    print("%d runs of >= %d (name, code) pairs\n" % (len(big), MIN_RUN))
    for r in big:
        names = [hits[va][0] for va in r]
        print("VA 0x%08x  %3d entries  e.g. %s" % (r[0], len(r), ", ".join(names[:6])))
    print()
    # Full dump of every run, for grepping.
    for r in sorted(big, key=lambda r: r[0]):
        print("== table @ 0x%08x (%d entries) ==" % (r[0], len(r)))
        for va in r:
            nm, code = hits[va]
            print("  0x%08x  %-44s sub_%08x" % (va, nm, code))


if __name__ == "__main__":
    main()

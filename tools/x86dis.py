#!/usr/bin/env python3
"""Disassemble guest x86 at a VA, straight out of the lift cache.

Every session that has had to answer "what does sub_XXXXXX actually do?" has
rewritten this script in a throwaway scratchpad. It reads `boot_cache/pe_sections.bin`
(the exact bytes the host maps at run time), so it needs neither Ghidra nor the
unwrapped exe, and it answers in a second.

    tools/x86dis.py 0x930390              # 24 instructions at that VA
    tools/x86dis.py 0x930390 -n 60        # more
    tools/x86dis.py 0x930390 --until-ret  # stop at the first ret
    tools/x86dis.py --xref 0xa97310       # every 4-byte little-endian reference
    tools/x86dis.py --calls 0x930390      # every `call rel32` landing on a VA

`--calls` matches the CALL by EXACT BYTES (E8 with the right displacement)
rather than disassembling linearly from the section base: .text carries data
islands, a linear sweep desyncs on them, and a previous session's linear scan
reported ZERO sites for a call that has sixteen.
"""
import argparse
import struct
import sys
import os

CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "boot_cache", "pe_sections.bin")


def load_sections(path=CACHE):
    """[(va, bytes)] exactly as the host maps them."""
    with open(path, "rb") as f:
        data = f.read()
    n = struct.unpack_from("<I", data, 0)[0]
    off, out = 4, []
    for _ in range(n):
        va, sz = struct.unpack_from("<II", data, off)
        off += 8
        out.append((va, data[off:off + sz]))
        off += sz
    return out


def read_at(sections, va, size):
    for base, raw in sections:
        if base <= va < base + len(raw):
            return raw[va - base:va - base + size]
    return b""


def disasm(sections, va, count, until_ret):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = read_at(sections, va, max(count * 16, 64))
    if not code:
        sys.exit(f"VA {va:#x} is in no section")
    shown = 0
    for ins in md.disasm(code, va):
        print(f"  {ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic:<8} {ins.op_str}")
        shown += 1
        if until_ret and ins.mnemonic.startswith("ret"):
            return
        if shown >= count:
            return


def xrefs(sections, target):
    needle = struct.pack("<I", target)
    for base, raw in sections:
        start = 0
        while True:
            i = raw.find(needle, start)
            if i < 0:
                break
            print(f"  dword ref at {base + i:#010x}")
            start = i + 1


def call_sites(sections, target):
    """Every `E8 rel32` whose destination is `target`, found by exact bytes."""
    for base, raw in sections:
        for i in range(len(raw) - 5):
            if raw[i] != 0xE8:
                continue
            rel = struct.unpack_from("<i", raw, i + 1)[0]
            if base + i + 5 + rel == target:
                print(f"  call at {base + i:#010x}")


def fn_bounds(sections, va):
    """(start, end) of the function containing va, by int3 padding.

    ⚠️ THIS EXISTS BECAUSE A FIXED-SIZE WINDOW LIES. Scanning "0x600 bytes from
    the function start" for referenced strings ran past the end of sub_4a0d20
    (which is 0x5c bytes long) and picked up `challengeResponse %i` from a
    neighbour 0x448 bytes away. I read that as "the null-vtable fault is in the
    connection handshake" and said so out loud before checking the string's
    actual xref, which is at 0x4a11a8 — a different function entirely. With real
    bounds, that function references NO strings at all.
    """
    start = va
    for p in range(va, va - 0x3000, -1):
        if read_at(sections, p - 2, 2) == b"\xcc\xcc":
            start = p
            break
    d = read_at(sections, start, 0x4000)
    end = start + 0x4000
    for i in range(4, len(d) - 1):
        if d[i] == 0xCC and d[i + 1] == 0xCC:
            end = start + i
            break
    return start, end


def strings_in_function(sections, va):
    """Every distinct C string referenced from INSIDE one function."""
    start, end = fn_bounds(sections, va)
    d = read_at(sections, start, end - start)
    seen, out = set(), []
    for i in range(max(0, len(d) - 4)):
        v = struct.unpack_from("<I", d, i)[0]
        if 0xA00000 < v < 0xC00000 and v not in seen:
            try:
                b = read_at(sections, v, 120).split(b"\x00")[0]
            except Exception:
                continue
            if len(b) >= 5 and all(32 <= c < 127 for c in b):
                seen.add(v)
                out.append((start + i, v, b.decode("latin1")))
    return start, end, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("va", nargs="?", help="virtual address, e.g. 0x930390")
    ap.add_argument("-n", type=int, default=24, help="instruction count")
    ap.add_argument("--until-ret", action="store_true")
    ap.add_argument("--strings", metavar="VA",
                    help="strings referenced from inside the function containing VA "
                         "(bounded by int3 padding — a fixed window overruns and lies)")
    ap.add_argument("--xref", help="find dword references to this VA")
    ap.add_argument("--calls", help="find `call rel32` sites targeting this VA")
    ap.add_argument("--cache", default=CACHE)
    a = ap.parse_args()
    sections = load_sections(a.cache)
    if a.strings:
        st, en, out = strings_in_function(sections, int(a.strings, 16))
        print(f"function {st:#x}..{en:#x} ({en - st:#x} bytes)")
        if not out:
            print("  (no strings referenced from inside this function)")
        for site, sva, txt in out:
            print(f"  {site:#08x} -> {sva:#08x}  {txt!r}")
    elif a.xref:
        xrefs(sections, int(a.xref, 0))
    elif a.calls:
        call_sites(sections, int(a.calls, 0))
    elif a.va:
        disasm(sections, int(a.va, 0), a.n, a.until_ret)
    else:
        ap.error("give a VA, --xref or --calls")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Find every instruction in .text that WRITES to a memory operand at a given
displacement -- e.g. the GSC pool's tag dword at `[base + var*8 + 4]`.

    tools/find_writers.py --disp 4              # every write to [... + 4]
    tools/find_writers.py --disp 4 --bit7       # only those that can SET bit 7
    tools/find_writers.py --disp 2 --control 0x578c04,0x5efdb6

Why not a linear sweep: .text carries data islands, an x86 linear disassembly
desyncs on them and renders kilobytes invisible -- a previous session published
a "zero" that way. So this decodes at EVERY byte offset independently. Bogus
decodes are the price; they are marked `~` (no aligned predecessor lands on
them) so a zero can never be a blind zero.

`--control` is MANDATORY discipline, not decoration: give it addresses you KNOW
match, and the tool exits non-zero if it fails to find them. A search that can
silently find nothing is a search that lies.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86dis import load_sections  # noqa: E402

# a destination memory operand with this displacement, i.e. `... ptr [<any> + N], ...`
def dest_re(disp):
    return re.compile(r"^(?:byte|word|dword|qword) ptr \[[^\]]*\+ 0x%x\]," % disp
                      if disp > 9 else
                      r"^(?:byte|word|dword|qword) ptr \[[^\]]*\+ %d\]," % disp)

WRITERS = {"mov", "or", "and", "xor", "add", "sub", "bts", "btr", "btc",
           "inc", "dec", "not", "neg", "xchg", "movzx", "movsx", "lock"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disp", type=lambda s: int(s, 0), default=4)
    ap.add_argument("--bit7", action="store_true",
                    help="keep only writes that can leave bit 7 of the low byte SET")
    ap.add_argument("--control", default="",
                    help="comma-separated VAs that MUST be found (positive control)")
    ap.add_argument("--section", default=".text")
    a = ap.parse_args()

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    secs = load_sections()
    # .text is the executable one; take the section covering the known code range
    text = [(va, b) for va, b in secs if va <= 0x401000 < va + len(b)]
    if not text:
        sys.exit("no section covers 0x401000")
    base, buf = text[0]
    print("scanning %s: 0x%06x..0x%06x  (%d bytes)" % (a.section, base, base + len(buf), len(buf)))

    rx = dest_re(a.disp)

    # pass 1: aligned instruction stream, so a hit can be marked plausible or not
    aligned = set()
    off = 0
    while off < len(buf):
        got = False
        for addr, size, _m, _o in md.disasm_lite(buf[off:off + 64], base + off, count=1):
            aligned.add(addr)
            off += size
            got = True
            break
        if not got:
            off += 1

    # pass 2: decode at EVERY offset; a data island cannot hide a match.
    # A cheap byte prefilter keeps this to seconds. It is a SUPERSET of the
    # encodings that can carry this displacement, so it cannot hide a match
    # either: mod=01 with disp8 == N, or mod=10 with disp32 == N, at any of the
    # offsets a 0F / 66 / F0 prefix can push the modrm to.
    d = a.disp

    def candidate(o):
        for mo in (1, 2, 3):                       # modrm at +1, or after one prefix
            if o + mo + 5 >= len(buf):
                continue
            m = buf[o + mo]
            dof = o + mo + 1 + (1 if (m & 7) == 4 else 0)
            if (m & 0xC0) == 0x40 and dof < len(buf) and buf[dof] == d:
                return True
            if (m & 0xC0) == 0x80 and dof + 4 <= len(buf) and \
               int.from_bytes(buf[dof:dof + 4], "little") == d:
                return True
        return False

    hits = []
    for off in range(len(buf) - 8):
        if not candidate(off):
            continue
        for addr, size, mn, ops in md.disasm_lite(buf[off:off + 16], base + off, count=1):
            if mn not in WRITERS or not rx.match(ops):
                break
            hits.append((addr, size, mn, ops))
            break

    ctrl = [int(x, 0) for x in a.control.split(",") if x.strip()]
    found = {h[0] for h in hits}
    for c in ctrl:
        print("  CONTROL 0x%06x: %s" % (c, "FOUND" if c in found else "*** MISSING ***"))

    def sets_bit7(mn, ops):
        m = re.search(r",\s*(0x[0-9a-f]+|-?\d+)$", ops)
        if m is None:
            return True                       # register source: unknown, keep it
        v = int(m.group(1), 0) & 0xFF
        if mn in ("or", "xor", "mov", "add"):
            return bool(v & 0x80)
        if mn == "and":
            return False                      # AND with imm cannot set a bit
        return True

    shown = [h for h in hits if (not a.bit7) or sets_bit7(h[2], h[3])]
    print("\n%d write(s) to [... + %d]%s, %d on the aligned stream:"
          % (len(shown), a.disp, " that can SET bit 7" if a.bit7 else "",
             sum(1 for h in shown if h[0] in aligned)))
    for addr, size, mn, ops in shown:
        print("  %s 0x%06x  %-6s %s" % (" " if addr in aligned else "~", addr, mn, ops))

    if ctrl and not all(c in found for c in ctrl):
        sys.exit("POSITIVE CONTROL FAILED - this search is blind, do not trust its zero")


if __name__ == "__main__":
    main()

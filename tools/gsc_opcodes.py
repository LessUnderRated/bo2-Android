#!/usr/bin/env python3
"""Dump the GSC VM's opcode dispatch table, statically, in one pass.

    tools/gsc_opcodes.py                # the whole table, one line per opcode
    tools/gsc_opcodes.py 0x17 0x54      # those opcodes, with disassembly
    tools/gsc_opcodes.py --calls        # every opcode's call targets, named

Why this exists: every "what does opcode 0xNN do?" has cost a disassembly
round-trip, and twice it cost a whole 10-minute run because the answer was
instrumented instead of read. The .text on disk is byte-identical to the live
image (measured: 0 of 6,489,264), so the table has always been readable offline.

The dispatch is `jmp dword[eax*4 + 0x883060]` out of the fetch at 0x87f878; the
VM function is sub_87f7c0..0x8833d1. Handlers that fall inside it are the
opcode bodies; a handler outside it is a shared tail.
"""
import argparse
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86dis import load_sections, disasm  # noqa: E402

TABLE = 0x00883060
NOPC = 124
VM_LO, VM_HI = 0x87F7C0, 0x8833D1


def reader(secs):
    def rd(va, n):
        for sva, b in secs:
            if sva <= va < sva + len(b):
                return b[va - sva:va - sva + n]
        return None
    return rd


def cstr(rd, va, cap=80):
    b = rd(va, cap)
    if not b:
        return None
    e = b.find(b"\0")
    if e <= 0:
        return None
    t = b[:e]
    return t.decode("latin1") if all(0x20 <= c < 0x7F for c in t) else None


def handler_strings(rd, secs, lo, hi):
    """Strings referenced by absolute address inside [lo, hi)."""
    out = []
    b = rd(lo, hi - lo)
    if not b:
        return out
    for i in range(len(b) - 4):
        v = struct.unpack_from("<I", b, i)[0]
        if 0xA00000 <= v < 0xC00000:
            s = cstr(rd, v)
            if s and len(s) > 3:
                out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("opcodes", nargs="*", help="opcodes to disassemble, e.g. 0x17")
    ap.add_argument("--calls", action="store_true", help="list each handler's call targets")
    ap.add_argument("-n", type=int, default=12, help="instructions per handler")
    a = ap.parse_args()

    secs = load_sections()
    rd = reader(secs)
    tbl = rd(TABLE, NOPC * 4)
    if not tbl:
        sys.exit("dispatch table not mapped — is boot_cache/pe_sections.bin present?")
    handlers = list(struct.unpack("<%dI" % NOPC, tbl))

    # a handler's body runs until the next handler address above it, or the VM end
    bounds = sorted(set(h for h in handlers if VM_LO <= h < VM_HI)) + [VM_HI]

    def body(h):
        for i, x in enumerate(bounds[:-1]):
            if x == h:
                return h, bounds[i + 1]
        return h, min(h + 0x60, VM_HI)

    want = {int(o, 0) for o in a.opcodes} if a.opcodes else None

    for op in range(NOPC):
        h = handlers[op]
        if want is not None and op not in want:
            continue
        inside = VM_LO <= h < VM_HI
        lo, hi = body(h) if inside else (h, h + 0x40)
        strs = handler_strings(rd, secs, lo, min(hi, lo + 0x400))
        tag = "" if inside else "   (outside the VM body — a shared tail)"
        note = ("  strings: " + ", ".join(repr(x) for x in strs[:3])) if strs else ""
        print("op 0x%02x -> 0x%06x  [0x%06x..0x%06x] %d bytes%s%s"
              % (op, h, lo, hi, hi - lo, tag, note))
        if want is None and not a.calls:
            continue
        buf = rd(lo, min(hi - lo, 0x400))
        if a.calls and buf:
            seen = []
            for i in range(len(buf) - 5):
                if buf[i] in (0xE8, 0xE9):
                    t = lo + i + 5 + struct.unpack_from("<i", buf, i + 1)[0]
                    if 0x401000 <= t < 0xA40000 and not (VM_LO <= t < VM_HI):
                        if t not in seen:
                            seen.append(t)
            for t in seen[:8]:
                print("      calls 0x%06x" % t)
        if want is not None:
            disasm(secs, h, a.n, False)
        print()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Disassemble any VA straight out of t6sp.exe — no run, no [DISPATCH-BYTES].

WHY THIS EXISTS. This project spent months decoding functions 32 bytes at a time
out of live guest memory, because GUIDE said the file was not the code (CEG).
Measured 2026-08-29: t6sp.exe's .text is byte-identical to the mapped image, all
6,489,264 of them. So the file IS the code and a disassembly costs a second
instead of an eight-minute instrumented run.

    tools/disasm.py 87e160          # 160 bytes from that VA
    tools/disasm.py 87e160 400      # a longer window
    tools/disasm.py --xref a92e78   # who references this address

⚠️ NOT named dis.py: that shadows the standard library's `dis`, which `inspect`
imports, and capstone then fails to load with a circular-import error.
"""
import os, sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = os.environ.get("BO2_EXE", "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe")   # BO2_EXE : autre exe (multi, Zombies)

def sections(raw):
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    n  = struct.unpack_from('<H', raw, pe + 6)[0]
    opt= struct.unpack_from('<H', raw, pe + 20)[0]
    base = struct.unpack_from('<I', raw, pe + 24 + 28)[0]
    out = []
    for i in range(n):
        o = pe + 24 + opt + i * 40
        name = raw[o:o+8].rstrip(b'\0').decode(errors='replace')
        vsz, va, rsz, ro = struct.unpack_from('<IIII', raw, o + 8)
        out.append((name, base + va, vsz, ro, rsz))
    return out

def va_to_off(secs, va):
    for name, sva, vsz, ro, rsz in secs:
        if sva <= va < sva + max(vsz, rsz):
            return ro + (va - sva)
    return None

def main():
    raw = open(EXE, 'rb').read()
    secs = sections(raw)
    if sys.argv[1] == '--calls':
        # WHO CALLS THIS. x86 calls are E8 rel32, so an absolute-address search
        # finds nothing — the reason --xref alone cannot answer "who calls
        # sub_XXXX". Scan .text for E8 whose computed target is the VA.
        target = int(sys.argv[2], 16)
        for name, sva, vsz, ro, rsz in secs:
            if name != '.text': continue
            hits = 0
            for k in range(rsz - 5):
                if raw[ro + k] != 0xE8: continue
                rel = struct.unpack_from('<i', raw, ro + k + 1)[0]
                if sva + k + 5 + rel == target:
                    print("  call at VA 0x%08x" % (sva + k)); hits += 1
            print("  %d direct call site(s)" % hits)
        return
    if sys.argv[1] == '--xref':
        target = int(sys.argv[2], 16)
        needle = struct.pack('<I', target)
        for name, sva, vsz, ro, rsz in secs:
            if name != '.text': continue
            at = ro
            hits = 0
            while True:
                at = raw.find(needle, at, ro + rsz)
                if at < 0: break
                print("  reference at VA 0x%08x" % (sva + (at - ro)))
                hits += 1; at += 1
            print("  %d reference(s) in .text" % hits)
        return
    va = int(sys.argv[1], 16)
    n  = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xa0
    off = va_to_off(secs, va)
    if off is None:
        print("VA 0x%08x is in no section" % va); return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for i in md.disasm(raw[off:off+n], va):
        print("  0x%08x: %-20s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))

main()

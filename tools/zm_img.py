#!/usr/bin/env python3
"""Image Zombies (boot_cache/zms/pe_sections.bin) : chaînes, références, désassemblage.
usage: zm_img.py str <texte>… | xref <va hex>… | dis <va hex> <octets>"""
import struct, sys
from pathlib import Path
import capstone
d = Path("boot_cache/zms/pe_sections.bin").read_bytes()
n, o, secs = struct.unpack_from("<I", d)[0], 4, []
for _ in range(n):
    va, l = struct.unpack_from("<II", d, o); secs.append((va, d[o + 8:o + 8 + l])); o += 8 + l
def lire(va, l):
    for b, r in secs:
        if b <= va < b + len(r): return r[va - b:va - b + l]
    return b""
def trouver(motif):
    for b, r in secs:
        i = r.find(motif)
        while i >= 0:
            yield b + i; i = r.find(motif, i + 1)
c = sys.argv[1]
if c == "str":
    for t in sys.argv[2:]:
        for va in trouver(t.encode()):
            deb = va
            while lire(deb - 1, 1) not in (b"\0", b""): deb -= 1
            print("%08x %r" % (deb, lire(deb, 80).split(b"\0")[0].decode("latin1")))
elif c == "xref":
    for h in sys.argv[2:]:
        print(h, [("%08x" % v) for v in trouver(struct.pack("<I", int(h, 16)))][:40])
elif c == "dis":
    va, l = int(sys.argv[2], 16), int(sys.argv[3], 0)
    for i in capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32).disasm(lire(va, l), va):
        print("%08x %-8s %s" % (i.address, i.mnemonic, i.op_str))
if c == "tab":                                         # tab <va> <n> : n pointeurs de chaînes
    va, k = int(sys.argv[2], 16), int(sys.argv[3])
    for j in range(k):
        p = struct.unpack("<I", lire(va + 4 * j, 4))[0]
        print("%08x -> %08x %r" % (va + 4 * j, p, lire(p, 60).split(b"\0")[0].decode("latin1")))

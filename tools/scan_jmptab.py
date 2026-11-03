#!/usr/bin/env python3
"""Recense les `jmp dword ptr [reg*4 + disp32]` dont la TABLE est dans .text.

C'est la signature exacte du gel du 2026-09-17 : la CRT liftee (memmove,
0x95f670) repartit son chemin « petite copie non alignee » par trois sauts
calcules a travers des tables de donnees logees dans .text. remill ne sait pas
suivre ca, donc l'invite tourne sur place. Le seul remede est d'interposer la
routine englobante.

Encodage vise : FF /4 avec ModRM=0x24 (SIB suit) et SIB scale=4, base=101
(disp32 absolu) -> FF 24 (0x85 | index<<3) disp32.
"""
import struct, sys, subprocess, re, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
EXE = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"

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

raw = open(EXE,'rb').read()
secs = sections(raw)
text = [s for s in secs if s[0] == '.text'][0]
_, tva, tvsz, tro, trsz = text
n = min(tvsz, trsz)
blob = raw[tro:tro+n]
lo, hi = tva, tva + n
REG = ['eax','ecx','edx','ebx','esp','ebp','esi','edi']

hits = []
i = 0
while True:
    i = blob.find(b'\xff\x24', i)
    if i < 0 or i + 7 > n: break
    sib = blob[i+2]
    # scale doit etre 4 (bits 7-6 == 0b10) et base == 101 (disp32)
    if (sib >> 6) == 2 and (sib & 7) == 5:
        tab = struct.unpack_from('<I', blob, i+3)[0]
        if lo <= tab < hi:                    # la TABLE est dans .text
            hits.append((tva + i, REG[(sib >> 3) & 7], tab))
    i += 1
print("%d sauts calcules dont la table est dans .text" % len(hits))
for va, reg, tab in hits:
    print("  0x%06x  jmp [%s*4 + 0x%06x]" % (va, reg, tab))

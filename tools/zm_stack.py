#!/usr/bin/env python3
"""Dernier relevé [GSTACK] d'un fil, filtré par l'image Zombies : ne garde que les adresses précédées d'un call.
usage: zm_stack.py <run.log> [tid]"""
import re, struct, sys
from pathlib import Path
d = Path("boot_cache/zms/pe_sections.bin").read_bytes()
n, o, secs = struct.unpack_from("<I", d)[0], 4, []
for _ in range(n):
    va, l = struct.unpack_from("<II", d, o); secs.append((va, d[o + 8:o + 8 + l])); o += 8 + l
def lire(va, l):
    for b, r in secs:
        if b <= va < b + len(r): return r[va - b:va - b + l]
    return b""
def appel(ret):
    """cible du call qui précède ret, ou None"""
    b = lire(ret - 7, 7)
    if len(b) < 7: return None
    if b[2] == 0xE8: return "%06x" % ((ret + struct.unpack("<i", b[3:7])[0]) & 0xFFFFFFFF)
    if b[5] == 0xFF and (b[6] >> 3) & 7 == 2: return "reg"
    if b[1] == 0xFF and b[2] == 0x15: return "[%x]" % struct.unpack("<I", b[3:7])[0]
    if b[4] == 0xFF and (b[5] >> 3) & 7 == 2 and b[5] >> 6 == 1: return "[reg+d8]"
    if b[1] == 0xFF and (b[2] >> 3) & 7 == 2 and b[2] >> 6 == 2: return "[reg+d32]"
    return None
tid = sys.argv[2] if len(sys.argv) > 2 else "0"
blocs, courant = [], None
for l in open(sys.argv[1], errors="replace"):
    if not l.startswith("[GSTACK]"): continue
    m = re.search(r"tid=(\d+)\s", l)
    if m: courant = [l[:150].strip()] if m.group(1) == tid else None; blocs += [courant] if courant else []
    elif courant is not None and "ret to" in l: courant.append(l)
b = blocs[-1]
print(len(blocs), "relevés ;", b[0])
for l in b[1:]:
    off, ret = re.search(r"\[esp\+(0x[0-9a-f]+)\] ret to (0x[0-9a-f]+)", l).groups()
    c = appel(int(ret, 16))
    if c: print("  esp+%s ret %s  <- call %s" % (off, ret, c))

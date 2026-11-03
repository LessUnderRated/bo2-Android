#!/usr/bin/env python3
"""C38 — replie les acces invites 8/16/64 bits, EN FLUX, sur stdin -> stdout.

Meme principe que stream_inline_writes.py : le code emis n'est pas recopie ici, il est
GENERE par tools/boot.py `_inline_guest_wide` sur un micro-module qui ne contient que
les `declare` effectivement vus dans le flux. Les lignes `declare` des intrinseques
repliees sont retirees ; les definitions internes (alwaysinline) sont ajoutees a la fin.
A placer AVANT dedup_ll_globals.py (qui retire les globaux re-declares).
Temoin : une ligne `[large] ...` sur stderr, avec les suffixes replies.
"""
import importlib.util
import re
import sys

spec = importlib.util.spec_from_file_location("bootpy", "tools/boot.py")
bootpy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootpy)

NOMS = ([("write", s) for s, *_ in bootpy._WIDE_WRITES] + [("read", s) for s, _ in bootpy._WIDE_READS])
DECL = re.compile(r'^\s*declare\b.*@__remill_(read|write)_memory_(8|16|64|f64)\(')
DEF = re.compile(r'^\s*define\b.*@__remill_(read|write)_memory_(8|16|64|f64)\(')
vus = {}
w = sys.stdout.write
for line in sys.stdin:
    m = DECL.match(line)
    if m and (m.group(1), m.group(2)) in NOMS:
        vus[(m.group(1), m.group(2))] = line
        continue
    if DEF.match(line):
        sys.stderr.write("[large] ❌ une definition de %s existe deja dans le flux — refus\n" % line.strip()[:120])
        sys.exit(3)
    w(line)
if not vus:
    sys.stderr.write("[large] 0 suffixe replie (aucun declare 8/16/64/f64 vu)\n")
    sys.exit(0)
micro = "".join(vus.values())
out = bootpy._inline_guest_wide(micro)
if out == micro or "\n\n" not in out:
    sys.stderr.write("[large] ❌ boot.py n'a rien replie sur le micro-module — refus\n")
    sys.exit(3)
genere = out.split("\n\n", 1)[1]
for piece in ("g_guest_writes", "bo2_w32_slow_map", "bo2_w32_cold"):
    if any(k == "write" for k, _ in vus) and piece not in genere:
        sys.stderr.write("[large] ❌ piece manquante : %s\n" % piece)
        sys.exit(3)
w("\n\n" + genere + "\n")
sys.stderr.write("[large] %d replie(s) : %s\n" % (len(vus), " ".join(f"{k}_{s}" for k, s in sorted(vus))))

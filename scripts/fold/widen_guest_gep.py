#!/usr/bin/env python3
"""Reelargir en i64 (par ZERO) l'index des GEP sur la base invitee.

POURQUOI. Le bitcode lie porte le datalayout d'i386 :
    target datalayout = "e-m:x-p:32:32-..."
La taille d'index y est donc de 32 bits, et `opt -O2` (tools/boot.py:663) a le
DROIT de reduire
    %az = zext i32 %a to i64 ; getelementptr i8, ptr %b, i64 %az
en
    getelementptr i8, ptr %b, i32 %a
C'est une canonicalisation legale sous p:32:32. Mais l'objet est ensuite
compile pour arm64, ou un index de GEP en i32 est ETENDU EN SIGNE (LangRef).
Resultat machine : `ldr w9, [x9, w1, sxtw]`. Pour toute adresse invitee dont
le bit 31 est mis (>= 0x80000000 — exactement le segment 2 de l'arene, la ou
vivent les piles des threads invites), l'adresse hote devient
    base + invite - 2^32
c'est-a-dire 4 Gio SOUS la carte, dans le garde PROT_NONE -> SIGBUS.

CE QUE FAIT CE FILTRE. Il relit le .ll final (apres toutes les passes d'opt,
donc rien ne peut re-reduire derriere lui) et remet le zext explicite.
Mesure : le datalayout n'est PAS touche, parce que sous les DEUX datalayouts
`zext i32->i64 + gep i64` donne `uxtw` et `gep i32` donne `sxtw`, a -O0 comme
a -O2. Le datalayout ne nuit que pendant `opt`.

COMMENT LA BASE EST RECONNUE. Pas par son nom (`%b.i357`) mais par sa
DEFINITION : on suit les `%X = load ptr, ptr @bo2_g_base`, remis a zero a
chaque `define`. Un `phi`/`select` de telles valeurs est suivi aussi.

🔴 PIEGE DEJA PAYE UNE FOIS. Un identifiant local LLVM non quote accepte
`[-a-zA-Z$._0-9]` : le TIRET en fait partie. Une premiere version utilisait
`[\w.]`, qui ratait `%.pre-phi8286` — 1 573 acces invites, dans 739 fonctions,
gardaient leur `sxtw`. Le residu s'est vu en desassemblant l'objet :
    adrp x8, _bo2_g_base ; ldr x8,[x8] ; ldr x8,[x8] ; ldr s0,[x8, w9, sxtw]
D'ou le FILET DE SECURITE ci-dessous : toute ligne `getelementptr` a index i32
dont la base est une base invitee connue mais que la regex principale n'a PAS
reconnue est comptee, et le filtre sort en erreur. Mieux vaut ne rien
installer qu'installer une correction partielle.
"""
import re, sys

NAME = r'[-\w.$]+'
GBASE_LOAD = re.compile(rf'^\s*%({NAME}) = load ptr, ptr @bo2_g_base\b')
GEP = re.compile(rf'^(\s*)%({NAME}) = getelementptr( inbounds| nuw| nusw)* i8, '
                 rf'ptr %({NAME}), i32 (%{NAME}|-?\d+)\s*$')
PHI = re.compile(rf'^\s*%({NAME}) = (?:phi ptr|select i1 [^,]+, ptr) (.*)$')
LOOSE = re.compile(rf'getelementptr[^,]*, ptr %({NAME}),\s*i32\b')
DEFINE = re.compile(r'^define\b')

gbase = set()
n_fix = n_const = n_other = n_left = 0
ex_left = ''
out = sys.stdout.write
for line in sys.stdin:
    if DEFINE.match(line):
        gbase.clear()
    m = GBASE_LOAD.match(line)
    if m:
        gbase.add(m.group(1)); out(line); continue
    m = PHI.match(line)
    if m and gbase:
        names = set(re.findall(rf'%({NAME})', m.group(2)))
        if names and names <= gbase:
            gbase.add(m.group(1))
        out(line); continue
    m = GEP.match(line)
    if m:
        ind, res, flags, base, idx = m.group(1), m.group(2), (m.group(3) or ''), m.group(4), m.group(5)
        if base in gbase:
            if idx.startswith('%'):
                tmp = f'%{res}.uxtw'
                out(f'{ind}{tmp} = zext i32 {idx} to i64\n')
                out(f'{ind}%{res} = getelementptr{flags} i8, ptr %{base}, i64 {tmp}\n')
                n_fix += 1
            else:
                out(f'{ind}%{res} = getelementptr{flags} i8, ptr %{base}, i64 {int(idx) & 0xffffffff}\n')
                n_const += 1
            continue
        n_other += 1
    else:
        # filet de securite : un GEP a index i32 sur la base invitee que la
        # regex principale n'a pas reconnu est une correction MANQUEE.
        lm = LOOSE.search(line)
        if lm and lm.group(1) in gbase:
            n_left += 1
            if not ex_left:
                ex_left = line.strip()[:120]
    out(line)

sys.stderr.write(f"[widen] {n_fix} GEP invites reelargis, {n_const} index constants, "
                 f"{n_other} GEP i8/i32 sur une AUTRE base (laisses tels quels)\n")
if n_left:
    sys.stderr.write(f"[widen] ❌ {n_left} GEP sur la BASE INVITEE gardent un index i32 "
                     f"(exemple : {ex_left}) — filtre incomplet, on n'installe rien.\n")
    sys.exit(3)

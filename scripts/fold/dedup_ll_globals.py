#!/usr/bin/env python3
"""Retirer les RE-declarations de symboles dans un flux .ll.

POURQUOI. `stream_inline_writes.py` ajoute son code genere a la FIN du module,
et ce code commence par re-declarer les six globaux dont il a besoin
(`@bo2_g_base`, `@bo2_w32_cold`, `@bo2_w32_slow_map`, `@g_guest_writes`,
`@g_w32_preempt_every`, `@g_w32_preempt_left`) plus trois `declare`. Or le
module liftе les declare DEJA. llvm-as refuse :

    error: redefinition of global '@bo2_g_base'

C'est ce qui a tue chain_writes.sh a 04:55 et chain_debit.sh a 05:14 — une
erreur de TEXTE, pas de memoire.

CE FILTRE EST VOLONTAIREMENT TIMIDE. Il ne supprime QUE :
  - une ligne `@nom = external global ...` dont le `@nom` a deja ete vu ;
  - une ligne `declare ... @nom(...)` dont le `@nom` a deja ete declare.
Il ne touche JAMAIS a un `define`, jamais a un global qui porte un
initialiseur, jamais a une premiere occurrence. Un `define` en doublon serait
une VRAIE erreur qu'il faut voir, pas cacher : le filtre s'arrete alors (code 3)
au lieu de la masquer.

TEMOIN NEGATIF : il imprime le nombre de doublons retires ET le nombre de
`define` vus deux fois, qui doit etre 0.
"""
import re
import sys

G_ANY   = re.compile(r'^@([A-Za-z0-9_.$\\-]+)\s*=')
G_EXT   = re.compile(r'^@([A-Za-z0-9_.$\\-]+)\s*=\s*external\s+')
DECLARE = re.compile(r'^declare\b.*?@([A-Za-z0-9_.$\\-]+)\s*\(')
DEFINE  = re.compile(r'^define\b.*?@([A-Za-z0-9_.$\\-]+)\s*\(')

vus_glob = set()
vus_decl = set()
vus_def = set()
n_glob = n_decl = n_def_double = 0
w = sys.stdout.write

for line in sys.stdin:
    m = G_ANY.match(line)
    if m:
        nom = m.group(1)
        if nom in vus_glob:
            # doublon : on ne l'accepte que si c'est une simple re-declaration
            if G_EXT.match(line):
                n_glob += 1
                continue
            sys.stderr.write(
                "[dedup] ❌ @%s redefini avec un initialiseur — ce n'est pas une\n"
                "        re-declaration anodine, je refuse de le cacher.\n" % nom)
            sys.exit(3)
        vus_glob.add(nom)
        w(line)
        continue

    m = DECLARE.match(line)
    if m:
        nom = m.group(1)
        if nom in vus_decl or nom in vus_def:
            n_decl += 1
            continue
        vus_decl.add(nom)
        w(line)
        continue

    m = DEFINE.match(line)
    if m:
        nom = m.group(1)
        if nom in vus_def:
            n_def_double += 1
            sys.stderr.write("[dedup] ❌ define @%s vu deux fois — vraie collision.\n" % nom)
            sys.exit(3)
        vus_def.add(nom)
        # un define l'emporte sur un declare anterieur : llvm-as l'accepte.
        w(line)
        continue

    w(line)

sys.stderr.write(
    "[dedup] %d re-declaration(s) de global retiree(s), %d declare en doublon "
    "retire(s) | temoin : %d define en doublon (doit etre 0)\n"
    % (n_glob, n_decl, n_def_double))

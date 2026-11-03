#!/usr/bin/env python3
"""Replie la barriere d'ecriture invitee, EN FLUX, sur stdin -> stdout.

POURQUOI PAS boot.py DIRECTEMENT. `_inline_guest_writes(ir_text)` prend tout le
module en une chaine et y fait des `re.sub`. Le `.ll` de ce projet fait
plusieurs GIGAOCTETS : sur une machine de 16 Gio ou macOS ne peut deja plus
creer de swap quand le disque est plein, c'est le scenario exact qui a
provoque un hard hang. `widen_guest_gep.py` a ete ecrit en flux pour la meme
raison ; celui-ci l'est aussi.

COMMENT ON RESTE BIT POUR BIT IDENTIQUE A boot.py. On ne recopie pas son code :
on l'IMPORTE et on l'appelle sur un module SYNTHETIQUE minuscule qui ne
contient que les deux `declare`. Ce que boot.py ajoute a la fin de ce
micro-module EST le code a emettre, verbatim. Si boot.py change un jour, ce
filtre change avec lui, sans copie a maintenir.

Le reste est purement local a la ligne, donc streamable :
  - toute ligne `declare ... @__remill_write_memory_{32,f32}(` est RETIREE
    (boot.py fait exactement ce `re.sub`), et note le suffixe comme utilise ;
  - a la fin, on ajoute les definitions generees.
"""
import sys, importlib.util, re

spec = importlib.util.spec_from_file_location("bootpy", "tools/boot.py")
bootpy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootpy)          # protege par `if __name__ == "__main__"`

MICRO = ('declare dso_local ptr @__remill_write_memory_32(ptr noundef, i32 noundef, i32 noundef)\n'
         'declare dso_local ptr @__remill_write_memory_f32(ptr noundef, i32 noundef, float noundef)\n')
out = bootpy._inline_guest_writes(MICRO)
if out == MICRO:
    sys.stderr.write("[ecrit] boot.py n'a RIEN replie sur le micro-module — "
                     "la signature des declare a change. On n'installe rien.\n")
    sys.exit(3)
# ce que boot.py a ajoute apres avoir retire les declare :
GENERE = out.split('\n\n', 1)[1] if '\n\n' in out else out
# controle de non-regression : le code genere doit contenir les quatre pieces
# dont l'omission a deja tue angola ou rendu l'ordonnanceur non preemptif.
for piece, quoi in (('g_guest_writes', 'le compteur dont cas_spin_note se sert'),
                    ('bo2_w32_slow_map', 'la carte de granule (TOUTE la surete)'),
                    ('bo2_w32_cold', 'la disjonction froide'),
                    ('win32_thread_write_tick', 'le tic de preemption')):
    if piece not in GENERE:
        sys.stderr.write("[ecrit] piece MANQUANTE dans le code genere : %s (%s)\n" % (piece, quoi))
        sys.exit(3)

DECL = re.compile(r'^\s*declare\b.*@__remill_write_memory_(32|f32)\(')
n_decl = 0
w = sys.stdout.write
for line in sys.stdin:
    if DECL.match(line):
        n_decl += 1
        continue
    w(line)
if n_decl == 0:
    sys.stderr.write("[ecrit] aucun declare @__remill_write_memory_{32,f32} trouve — "
                     "rien a replier, on n'installe rien.\n")
    sys.exit(3)
w("\n\n" + GENERE + "\n")
sys.stderr.write("[ecrit] %d declare retire(s), voie rapide d'ecriture repliee "
                 "(code genere par boot.py lui-meme, %d octets)\n" % (n_decl, len(GENERE)))

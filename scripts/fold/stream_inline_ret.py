#!/usr/bin/env python3
"""Android — replie le `ret` invite (`__remill_function_return`), EN FLUX, sur stdin -> stdout.

Meme construction que stream_inline_triv.py : on IMPORTE tools/boot.py et on appelle `_inline_guest_ret` sur un
micro-module qui ne contient que le `declare` retire ; ce que boot.py ajoute EST le code emis.

Pourquoi maintenant : fold_lift.sh laissait ce repli de cote, parce que la voie courte du repli ne faisait pas le
test du code du tas (P725/P729) que fait l'entree complete. Le repli teste desormais que la cible est dans l'image
([0x400000, 0x43bc000) : jamais du code du tas, bo2_est_heap_code) ; toute autre cible, les sondes de ret
(bo2_ret_cold != 0), un esp sauvage et la cible 0x883440 prennent l'entree complete comme avant. Sur le S26 chaque
`ret` invite appelait cette entree : 3 a 4 % du fil ordonnanceur, le goulot.
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("bootpy", "tools/boot.py")
bootpy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootpy)          # protege par `if __name__ == "__main__"`

NOM = "@__remill_function_return("
declare = None
refs = 0
sortie = sys.stdout
for ligne in sys.stdin:
    if NOM in ligne:
        if ligne.lstrip().startswith("declare"):
            declare = ligne
            continue
        if ligne.startswith("define"):
            sys.stderr.write("[ret] le module DEFINIT deja __remill_function_return — on n'installe rien.\n")
            sys.exit(3)
        refs += ligne.count(NOM)
    sortie.write(ligne)

if refs and declare is None:
    sys.stderr.write("[ret] references sans declare — on n'installe rien.\n")
    sys.exit(3)
if declare is not None:
    out = bootpy._inline_guest_ret(declare)
    if out == declare or "\n\n" not in out:
        sys.stderr.write("[ret] boot.py n'a rien replie sur le micro-module.\n")
        sys.exit(3)
    genere = out.split("\n\n", 1)[1]
    if "horsimg" not in genere:
        sys.stderr.write("[ret] le repli genere n'a pas le test de l'image (code du tas) — on n'installe rien.\n")
        sys.exit(3)
    sortie.write("\n" + genere)
sys.stderr.write(f"[ret] declare {'replie' if declare else 'absent'}, {refs} references\n")

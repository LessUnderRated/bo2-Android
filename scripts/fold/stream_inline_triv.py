#!/usr/bin/env python3
"""C26 — replie les intrinseques INERTES de remill, EN FLUX, sur stdin -> stdout.

Meme construction que stream_inline_writes.py : on n'ecrit pas les corps ici, on
IMPORTE tools/boot.py et on appelle `_inline_trivial_intrinsics` (BO2_INLINE_TRIV)
sur un micro-module qui ne contient que les `declare` retires. Ce que boot.py
ajoute a la fin de ce micro-module EST le code emis, verbatim.

Familles repliees (toutes identite, constante ou vide dans win32_runtime.cpp) :
  __remill_compare_{eq,neq,ugt,uge,ult,ule,sgt,sge,slt,sle}   identite
  __remill_undefined_{8,16,32,64,f32,f64,f80}                  0
  __remill_fpu_exception_{clear,test}, __remill_fpu_{get,set}_rounding   vide / 0
Declarees `noinline optnone` sans attribut memoire, ce sont des barrieres
completes pour l'optimiseur : l'etat (registres, drapeaux, fentes x87) doit etre
ecrit avant chacune et relu apres. Les acces a la memoire INVITEE restent
`volatile` (replis de lecture et d'ecriture), donc aucun ordre invite ne bouge.

Local a la ligne : une ligne `declare ... @<cible>(` est retiree et gardee ; toute
autre ligne passe telle quelle, en comptant les references aux cibles. A la fin,
une cible referencee sans `declare` fait echouer le filtre (sortie 3), sinon on
ajoute les definitions.
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("bootpy", "tools/boot.py")
bootpy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootpy)          # protege par `if __name__ == "__main__"`

CIBLES = ([f"__remill_compare_{c}" for c in bootpy._CMP]
          + [f"__remill_undefined_{s}" for s, *_ in bootpy._UNDEF]
          + [nom for nom, *_ in bootpy._FPU])
MOTIFS = [(nom, f"@{nom}(") for nom in CIBLES]

declares: dict[str, str] = {}
refs: dict[str, int] = {nom: 0 for nom in CIBLES}
sortie = sys.stdout
for ligne in sys.stdin:
    if "@__remill_" in ligne:
        cible = next((nom for nom, m in MOTIFS if m in ligne), None)
        if cible is not None and ligne.lstrip().startswith("declare"):
            declares[cible] = ligne
            continue
        for nom, m in MOTIFS:
            if m in ligne:
                refs[nom] += ligne.count(m)
    sortie.write(ligne)

orphelins = [nom for nom, n in refs.items() if n and nom not in declares]
if orphelins:
    sys.stderr.write(f"[triv] references sans declare : {orphelins} — on n'installe rien.\n")
    sys.exit(3)
if declares:
    micro = "".join(declares.values())
    out = bootpy._inline_trivial_intrinsics(micro)
    if out == micro or "\n\n" not in out:
        sys.stderr.write("[triv] boot.py n'a rien replie sur le micro-module.\n")
        sys.exit(3)
    genere = out.split("\n\n", 1)[1]
    ndef = genere.count("define internal")
    if ndef != len(declares):
        sys.stderr.write(f"[triv] {len(declares)} declare retires mais {ndef} definitions.\n")
        sys.exit(3)
    sortie.write("\n" + genere)
sys.stderr.write(f"[triv] {len(declares)} declare replies, "
                 f"{sum(refs.values())} references\n")

#!/bin/bash
# wave_closure.sh <wave-number> — fermer les trous que la vague vient de créer.
#
# Lifter une VA isolée dont le bloc se termine par un saut direct produit un
# __remill_missing_block sur la cible : la vague 32 a lifté 0x5090e5 (« jmp
# 0x5090a6 ») et le run suivant est mort sur « [MISSING] block at 0x005090a6 ».
# Le moissonneur lit ça dans l'IR de la vague, sans run.
#
# ⚠️ CE N'EST PAS scripts/lift_closure.sh, qui relance le lift COMPLET et est un
# piège prouvé (docs/SPEED.md étape 69). Ici on ne touche qu'à la liste de la
# vague, et on ne relifte que les VA ajoutées.
#
#   bash scripts/wave_closure.sh 33      # itère jusqu'au point fixe
set -euo pipefail
cd "$(dirname "$0")/.."
W="${1:?usage: wave_closure.sh <wave-number>}"
LIST="tools/extra_lifted_vas_wave${W}.txt"
[ -f "$LIST" ] || { echo "no such wave list: $LIST" >&2; exit 2; }

for pass in 1 2 3 4 5; do
    python3 tools/harvest_missing_blocks.py --workdir boot_cache/lift_wip \
            --out /tmp/wave_closure_$$.txt
    # garder ce qui n'est ni déjà dans la liste ni déjà un point d'entrée lifté
    NEW=$(python3 - "$LIST" /tmp/wave_closure_$$.txt "$W" <<'PY'
import glob, os, sys, subprocess
have = {l.split('#')[0].strip().lower() for l in open(sys.argv[1]) if l.split('#')[0].strip()}
have = {int(h, 16) for h in have}
# Les points d'entree deja liftes se lisent dans les OBJETS, pas dans le
# binaire final : `nm -U boot_cache/boot` rendait un filtre VIDE et MUET quand
# un lien avait echoue, et la vague 33 est repartie avec deux VA deja liftees
# par les vagues 22/23 -> « duplicate symbol '_sub_5536c8' » au lien.
# 🔴 2026-09-18 : ce glob ne voyait que lifted.o + extra_lifted*.o. Or
# boot_cache/unwind_extra.o porte 864 symboles _sub_*, invisibles ici — la
# vague 40 a donc re-lifte 10 VA deja presentes et le lien a rendu
# « ld: 10 duplicate symbols », SANS produire de binaire, pendant que
# scripts/build_only.sh rendait 0. On enumere TOUS les objets.
objs = [o for o in glob.glob("boot_cache/*.o")
        if not o.endswith("_wave%s.o" % sys.argv[3])]
objs = [o for o in objs if os.path.exists(o)]
if not objs:
    sys.exit("wave_closure: aucun objet lifte a interroger — abandon plutot "
             "qu'un filtre vide")
ent = set()
for o in objs:
    nm = subprocess.run(["nm", "-U", o], capture_output=True, text=True).stdout
    ent |= {int(w[5:], 16) for w in nm.split()
            if w.startswith("_sub_") and all(c in "0123456789abcdef" for c in w[5:])}
out = []
for l in open(sys.argv[2]):
    l = l.strip()
    if not l:
        continue
    va = int(l, 16)
    if va not in have and va not in ent:
        out.append(va)
print(" ".join("0x%06x" % v for v in sorted(out)))
PY
)
    rm -f /tmp/wave_closure_$$.txt
    if [ -z "$NEW" ]; then echo "[closure] point fixe atteint en $((pass-1)) passe(s)"; exit 0; fi
    echo "[closure] passe $pass : +$(echo "$NEW" | wc -w) VA -> $NEW"
    for v in $NEW; do echo "$v" >> "$LIST"; done
    python3 tools/lift_extra.py --list "$LIST" \
            --obj "boot_cache/extra_lifted_wave${W}.o" \
            --inc "runtime/win32/extra_lifted_vas_wave${W}.inc"
done
echo "[closure] ⚠️ pas de point fixe en 5 passes — regarder $LIST a la main" >&2
exit 1

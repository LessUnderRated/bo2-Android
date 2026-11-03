#!/usr/bin/env bash
# lift_closure.sh — fermer les DEUX classes « pas lifte » jusqu'a point fixe.
#
# Classe 1 — [MISSING] block at 0xVA not lifted : remill a refuse de suivre un
#   saut DIRECT et a emis __remill_missing_block. tools/harvest_missing_blocks.py
#   lit ces sites dans les .bc.fixed, donc la classe se recense STATIQUEMENT.
#   (C'est ce qui a tue runs/long1 a 1005 s, apres 17 minutes de jeu.)
# Classe 2 — [STUB-CALL] : une fonction REFERENCEE mais pas liftee recoit un stub
#   `call @__stub_call_fatal`. boot.py les liste dans
#   boot_cache/undefined_stubs.txt a chaque passe.
#
# Les deux se referment de la meme facon : ajouter les VA aux racines et relancer.
# La relance est bon marche — les .fixed deja produits sont reutilises tels quels,
# seules les NOUVELLES fonctions passent par remill.
#
# ⚠️ La liste de racines DOIT rester celle reconstruite depuis le binaire de
# reference (tools/lift_roots_ref40k.txt) : calltree.csv + lift_roots_combined.txt
# seuls ne donnent que 14 658 fonctions contre 37 264 en service.
set -euo pipefail
cd "$(dirname "$0")/.."

WORK="${WORK:-boot_cache/lift_full}"
ROOTS="${ROOTS:-tools/lift_roots_ref40k.txt}"
MAX="${MAX:-6}"
JOBS="${JOBS:-6}"

for i in $(seq 1 "$MAX"); do
  echo "=== passe $i/$MAX ==="
  n_before=$(grep -c '^0x' "$ROOTS")

  # classe 1 : blocs abandonnes, lus dans l'IR deja produite
  python3 tools/harvest_missing_blocks.py --workdir "$WORK" --jobs 8 \
      > /tmp/lc_missing.txt 2>/tmp/lc_missing.err || true
  # classe 2 : stubs fatals de la passe precedente
  cp -f boot_cache/undefined_stubs.txt /tmp/lc_stubs.txt 2>/dev/null || : > /tmp/lc_stubs.txt

  python3 - "$ROOTS" /tmp/lc_missing.txt /tmp/lc_stubs.txt <<'PY'
import sys, io
roots = sys.argv[1]
have = set()
for l in io.open(roots):
    t = l.split('#')[0].strip()
    if t:
        try: have.add(int(t, 16))
        except ValueError: pass
new = set()
for f in sys.argv[2:]:
    try: txt = io.open(f).read()
    except OSError: continue
    for l in txt.split():
        try: v = int(l, 16)
        except ValueError: continue
        if 0x401000 <= v < 0xa3b000 and v not in have:
            new.add(v)
if new:
    with io.open(roots, 'a') as f:
        f.write("# +%d VA ajoutees par scripts/lift_closure.sh\n" % len(new))
        for v in sorted(new): f.write("0x%x\n" % v)
print("%d nouvelle(s) racine(s)" % len(new))
PY

  n_after=$(grep -c '^0x' "$ROOTS")
  if [ "$n_after" -eq "$n_before" ]; then
    echo "POINT FIXE atteint a la passe $i : plus aucun bloc abandonne ni stub fatal."
    exit 0
  fi
  echo "racines $n_before -> $n_after ; relance du lift"
  BOOT_WORKDIR="$WORK" BOOT_BC_SRC="$WORK" LIFT_ROOTS_FILE="$ROOTS" \
    BOOT_OPT="${BOOT_OPT:--O0}" BOOT_COMPILE_TIMEOUT="${BOOT_COMPILE_TIMEOUT:-3600}" \
    python3 -u tools/boot.py -n 10036 -j "$JOBS"
done
echo "⚠️ $MAX passes sans point fixe — regarder /tmp/lc_missing.txt"
exit 1

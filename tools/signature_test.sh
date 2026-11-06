#!/usr/bin/env bash
# Auto-controle de scripts/signature.sh check dans un repertoire temporaire.
set -euo pipefail
S="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/signature.sh"
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
mkdir -p "$D/scripts" "$D/runtime" "$D"/boot_cache/{sps,zms,mps}; cp "$S" "$D/scripts/"; echo x > "$D/runtime/a.cpp"; cd "$D"
s() { bash scripts/signature.sh "$@"; }
ko() { echo "ECHEC : $*" >&2; exit 1; }
for t in sps zms mps; do s lift $t > boot_cache/$t/.sig_lift; s link $t > boot_cache/$t/.sig_link; touch boot_cache/$t/.fini; done
s check                                  || ko "conversion a jour vue perimee"
rm boot_cache/zms/.fini
! s check                                || ko "relift zms commence et non fini vu a jour"
echo sps > boot_cache/conversion_target
s check                                  || ko "mode hors cible verifie"
rm boot_cache/conversion_target; touch boot_cache/zms/.fini; echo y >> runtime/a.cpp
! s check                                || ko "runtime change non vu"
rm boot_cache/*/.sig_*
s check                                  || ko "conversion sans signature refaite"
echo OK

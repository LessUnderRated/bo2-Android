#!/usr/bin/env bash
# install_lift.sh <workdir> — installer un lifted.o fraichement produit, APRES
# avoir verifie les deux choses qui cassent un lien de 271 objets :
#   1. la couverture  (compter les _sub_ : un lift trop etroit est un instrument
#      casse, pas un binaire lent — cf. docs/SPEED.md etape 69) ;
#   2. l'absence de collision avec les ~38 000 _sub_ que les extra_lifted*.o et
#      *_impl.o definissent deja (« duplicate symbol » sinon).
# Le lien lui-meme est fait par scripts/build_only.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

WORK="${1:?usage: install_lift.sh <workdir>   (ex: boot_cache/lift_full)}"
NEW="$WORK/lifted.o"
EXCL="${EXCL:-tools/lift_exclude_provided.txt}"
MIN="${MIN:-35000}"

[ -f "$NEW" ] || { echo "❌ $NEW absent — le lift n'a pas produit d'objet"; exit 1; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
nm "$NEW" | awk '$2=="T" && $3 ~ /^_sub_[0-9a-f]+$/ {print substr($3,6)}' | sort -u > "$tmp/new.txt"
n_new=$(wc -l < "$tmp/new.txt")
echo "[install] $NEW definit $n_new fonctions"
if [ "$n_new" -lt "$MIN" ]; then
  echo "❌ moins de $MIN fonctions — liste d'entree trop courte, RIEN n'est installe."
  echo "   Reconstruire les racines depuis le binaire de reference (tools/lift_roots_ref40k.txt)."
  exit 1
fi

grep '^0x' "$EXCL" | sed 's/^0x//' | sort -u > "$tmp/excl.txt"
n_col=$(comm -12 "$tmp/new.txt" "$tmp/excl.txt" | tee "$tmp/col.txt" | wc -l)
if [ "$n_col" -gt 0 ]; then
  echo "❌ $n_col symbole(s) deja fourni(s) par un autre objet du lien :"
  head -5 "$tmp/col.txt" | sed 's/^/   _sub_/'
  echo "   Relancer le lift avec LIFT_EXCLUDE_FILE=$EXCL"
  exit 1
fi
echo "[install] 0 collision avec les 38 027 VA fournies ailleurs"

STAMP=$(date +%Y%m%d-%H%M%S)
cp -c boot_cache/lifted.o "boot_cache/lifted.o.$STAMP" 2>/dev/null || cp boot_cache/lifted.o "boot_cache/lifted.o.$STAMP"
echo "[install] ancien objet garde sous boot_cache/lifted.o.$STAMP"
cp "$NEW" boot_cache/lifted.o
echo "[install] installe. Lancer maintenant : bash scripts/build_only.sh"

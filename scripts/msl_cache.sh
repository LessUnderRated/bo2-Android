#!/bin/bash
# msl_cache.sh — compile en metallib les shaders que le runtime a déposés dans boot_cache/msl_cache (P770).
# Au run suivant, le runtime charge <fnv64>.metallib (0,1 ms) au lieu de compiler la source (144 ms à froid) :
# il ne dépend plus du cache Metal de macOS, qui jette des générations entières. Hors run (CPU).
# Sans la chaîne d'outils Metal (xcrun metal), ne fait rien : le runtime compile la source comme avant.
set -u
cd "${1:-$(dirname "$0")/../boot_cache/msl_cache}" 2>/dev/null || exit 0   # $1 : autre dossier (ex. boot_cache/zms/msl_cache)
# xcode-select peut pointer sur CommandLineTools (sans metal) : essayer alors chaque Xcode installé.
M=$(xcrun -sdk macosx -f metal 2>/dev/null) ||
    for x in /Applications/Xcode*.app; do M=$(DEVELOPER_DIR="$x/Contents/Developer" xcrun -sdk macosx -f metal 2>/dev/null) && break; done
[ -n "${M:-}" ] || { echo "[msl_cache] pas de xcrun metal" >&2; exit 0; }
for f in *.msl; do [ -e "$f" ] && [ ! -e "${f%.msl}.metallib" ] && echo "${f%.msl}"; done |
    xargs -P8 -I{} sh -c '"$0" -x metal -fpreserve-invariance {}.msl -o {}.tmp && mv {}.tmp {}.metallib || rm -f {}.tmp' "$M"
echo "[msl_cache] $(ls *.metallib 2>/dev/null | wc -l | tr -d ' ') metallib, $(ls *.msl 2>/dev/null | wc -l | tr -d ' ') sources"

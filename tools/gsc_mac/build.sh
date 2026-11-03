#!/bin/sh
# Compile gsc-tool (xensik, GPL-3.0, étiquette 1.6.0) sur macOS : le compilateur des scripts GSC/CSC de T6.
# Prérequis : `brew install premake`, et le clone (ignoré par git, ~98 Mo avec ses sous-modules) :
#   git clone --recurse-submodules --shallow-submodules --depth 1 -b 1.6.0 \
#       https://github.com/xensik/gsc-tool tools/gsc-tool-src                 # corrigé pour le commit 1edac5a
# Résultat : tools/gsc-tool-src/build/bin/arm64/release/gsc-tool, ~2 min.
# getdvarint.patch : en 1.6.0 l'analyseur range l'argument de getdvarint()/getdvarfloat() dans une liste que le
# compilateur ne sait pas émettre (« unknown expression ») ; le correctif émet les éléments de la liste.
set -e
ICI=$(cd "$(dirname "$0")" && pwd)
cd "$ICI/../gsc-tool-src"
git apply --check "$ICI/getdvarint.patch" 2>/dev/null && git apply "$ICI/getdvarint.patch"   # déjà appliqué : on passe
premake5 gmake
cd build
make -j6 config=release_arm64 xsk-tool
# contrôle : un getdvarint doit compiler (il échouait avant le correctif)
T=$(mktemp -d)
mkdir -p "$T/s"
printf 'main()\n{\n    a = getdvarint( "x" );\n}\n' > "$T/s/essai.gsc"
(cd "$T" && "$ICI/../gsc-tool-src/build/bin/arm64/release/gsc-tool" -m comp -g t6 -s pc s) && test -s "$T/compiled/t6/essai.gsc" && echo "gsc-tool : OK"

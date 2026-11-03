#!/bin/sh
# Compile les règles de partie de runtime/match_rules/{mp,zm}/ vers /tmp/match_rules/{mp,zm}.gsc (gsc-tool, tools/gsc_mac/build.sh)
# et runtime/match_rules/{mp,zm}.inc, embarqué par runtime/win32/match_rules.cpp : à relancer quand un script change.
cd "$(dirname "$0")/../.." || exit 1
G=$PWD/tools/gsc-tool-src/build/bin/arm64/release/gsc-tool; R=$PWD/runtime/match_rules
mkdir -p /tmp/match_rules
for m in ${1:-mp zm}; do
  [ -d "$R/$m" ] || continue
  (cd "$R" && "$G" -m comp -g t6 -s pc -w "$R/$m" "$m" 2>&1 | grep -v '^$' | tail -6)
  f=$(find "$R/compiled" -name '_globallogic.gsc' | head -1); [ -n "$f" ] && cp "$f" /tmp/match_rules/$m.gsc && ls -la /tmp/match_rules/$m.gsc && (cd /tmp/match_rules && xxd -i < $m.gsc) > "$R/$m.inc"
  rm -rf "$R/compiled"
done

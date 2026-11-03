#!/bin/bash
# boot_named.sh <boot> <Info.plist> — prints a path that runs <boot> from inside a bundle, so that macOS (FineTune, the
# Dock, Activity Monitor) names the game from the plist instead of "boot": LaunchServices ignores the __info_plist
# section of a bare executable. Hard link (same file, no copy), redone at every launch since a relink replaces boot.
# The game finds msl_cache next to its executable: a link points back to the real one.
B="$1"; A="$(dirname "$B")/Black Ops II.app/Contents"
mkdir -p "$A/MacOS" && cp "$2" "$A/Info.plist" && ln -f "$B" "$A/MacOS/boot" \
  && ln -sfn ../../../msl_cache "$A/MacOS/msl_cache" && echo "$A/MacOS/boot" || echo "$B"

#!/usr/bin/env bash
# scripts/upstream.sh — the runtime tree the Android build compiles, and the sources its headers come from.
#
#   android/build/upstream/bo2-mac   this repository's committed tree (the bo2-mac runtime with the Android work)
#   android/build/upstream/remill    Remill v6.0.1 (headers only: the x86 State the runtime and the lift share)
#   android/build/upstream/unicorn   Unicorn 2.1.3 (the x86 VM for heap code and exception handlers)
#   android/build/upstream/libvpx    libvpx v1.15.2 (the game's VP8 videos)
#
# Nothing from the game is fetched or written here. Re-run after committing a runtime change; it starts clean.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=android/build/upstream
mkdir -p "$OUT"

rm -rf "$OUT/bo2-mac"
mkdir -p "$OUT/bo2-mac"
# LF as committed, whatever core.autocrlf says on this machine.
git -c core.autocrlf=false archive HEAD -- . ':!android' | tar -x -C "$OUT/bo2-mac"
echo "runtime $(git rev-parse --short HEAD) -> $OUT/bo2-mac"

fetch() {   # name tag url
    if [ ! -d "$OUT/$1/.git" ]; then
        git -c core.autocrlf=false -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$3" "$OUT/$1"
    fi
    echo "$1 $(git -C "$OUT/$1" describe --tags --always 2>/dev/null)"
}
fetch remill v6.0.1 https://github.com/lifting-bits/remill
fetch unicorn 2.1.3 https://github.com/unicorn-engine/unicorn
fetch libvpx v1.15.2 https://chromium.googlesource.com/webm/libvpx

#!/bin/bash
# build_remill.sh — patched Remill v6.0.1 (~12 min the first time, ~0.7 GB). Used by build_steam.sh.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD; BC=boot_cache; R=tools/remill-src; J=${J:-2}
# LLVM 17: Homebrew's on macOS; LLVM17_PREFIX elsewhere (Linux: /usr/lib/llvm-17, for the Android lift).
L17=${LLVM17_PREFIX:-/opt/homebrew/opt/llvm@17}; EXTRA=${LLVM17_PREFIX:+$LLVM17_PREFIX}; EXTRA=${EXTRA:-/opt/homebrew}
[ ! -x $R/build/bin/lift/remill-lift-17 ] || [ ! -f $R/build/lib/Arch/X86/Runtime/x86.bc ] || exit 0
[ -d $R/.git ] || git clone -q --depth 1 --branch v6.0.1 https://github.com/lifting-bits/remill $R
for p in tools/remill-patches/*.patch; do
  (cd $R && git apply --reverse --check "$ROOT/$p" 2>/dev/null) || (cd $R && git apply "$ROOT/$p")
done
cd $R
cmake -S dependencies -B build-deps -G Ninja -DUSE_EXTERNAL_LLVM=ON \
      -DCMAKE_PREFIX_PATH="$L17;$EXTRA" \
      -DCMAKE_INSTALL_PREFIX=$PWD/deps-install -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build-deps -j$J > "$ROOT/$BC/remill.log"   # ninja's [n/N] lines: the window shows them
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$PWD/deps-install;$L17;$EXTRA" > /dev/null
cmake --build build -j$J --target remill-lift-17 >> "$ROOT/$BC/remill.log"
ninja -C build lib/Arch/X86/Runtime/x86.bc > /dev/null   # not a dependency of remill-lift-17

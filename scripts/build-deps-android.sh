#!/usr/bin/env bash
# scripts/build-deps-android.sh — Unicorn and libvpx as static arm64 libraries for the game core (Linux or WSL).
#
#   scripts/build-deps-android.sh <android NDK r28c for Linux> <output dir>
#
# Unicorn 2.1.3, x86 only: the VM that runs the game's integrity checks and its heap code (bo2-mac's x86_guest_vm.cpp,
# heap_code.cpp). Its build runs qemu's configure, which is why this is a Linux script. libvpx v1.15.2, the VP8
# decoder only (the game's videos), with NEON. Sources from scripts/upstream.sh (android/build/upstream).
set -euo pipefail
NDK="${1:?usage: build-deps-android.sh <ndk> <out>}"
OUT="${2:?usage: build-deps-android.sh <ndk> <out>}"
cd "$(dirname "$0")/.."
UP=$PWD/android/build/upstream
[ -d "$UP/unicorn" ] && [ -d "$UP/libvpx" ] || { echo "run scripts/upstream.sh first" >&2; exit 1; }
API=33
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
WORK=${DEPS_WORK:-$HOME/bo2-deps}
mkdir -p "$WORK" "$OUT/lib" "$OUT/include"
J=$(nproc)

# ---- Unicorn ---------------------------------------------------------------------------------------------------------
if [ ! -f "$OUT/lib/libunicorn.a" ]; then
    rm -rf "$WORK/unicorn-src" "$WORK/unicorn-build"
    cp -r "$UP/unicorn" "$WORK/unicorn-src"
    cmake -S "$WORK/unicorn-src" -B "$WORK/unicorn-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=x86 \
        -DUNICORN_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF -DUNICORN_LEGACY_STATIC_ARCHIVE=ON > "$WORK/unicorn-cmake.log" 2>&1 \
        || { tail -20 "$WORK/unicorn-cmake.log"; exit 1; }
    cmake --build "$WORK/unicorn-build" -j "$J" > "$WORK/unicorn-build.log" 2>&1 || { tail -30 "$WORK/unicorn-build.log"; exit 1; }
    # UNICORN_LEGACY_STATIC_ARCHIVE: libunicorn.a is the whole library (API, common code, the x86 back end); the
    # other archives are its pieces and would define every symbol twice.
    cp "$WORK/unicorn-build/libunicorn.a" "$OUT/lib/libunicorn.a"
    cp -r "$UP/unicorn/include/unicorn" "$OUT/include/"
    echo "unicorn: $(ls -la "$OUT/lib/libunicorn.a" | awk '{print $5}') bytes"
fi

# ---- libvpx ----------------------------------------------------------------------------------------------------------
if [ ! -f "$OUT/lib/libvpx.a" ]; then
    rm -rf "$WORK/libvpx-build"
    mkdir -p "$WORK/libvpx-build"
    cd "$WORK/libvpx-build"
    CC="$TC/aarch64-linux-android$API-clang" CXX="$TC/aarch64-linux-android$API-clang++" AR="$TC/llvm-ar" \
    AS="$TC/aarch64-linux-android$API-clang" LD="$TC/aarch64-linux-android$API-clang" STRIP="$TC/llvm-strip" \
    NM="$TC/llvm-nm" RANLIB="$TC/llvm-ranlib" \
        "$UP/libvpx/configure" --target=arm64-linux-gcc --enable-pic --disable-examples --disable-tools --disable-docs \
            --disable-unit-tests --disable-vp9 --disable-vp8-encoder --enable-vp8-decoder --disable-webm-io \
            --disable-libyuv --enable-runtime-cpu-detect > "$WORK/libvpx-configure.log" 2>&1 \
        || { tail -20 "$WORK/libvpx-configure.log"; exit 1; }
    make -j "$J" > "$WORK/libvpx-build.log" 2>&1 || { tail -30 "$WORK/libvpx-build.log"; exit 1; }
    cp libvpx.a "$OUT/lib/"
    mkdir -p "$OUT/include/vpx"
    cp "$UP"/libvpx/vpx/*.h "$OUT/include/vpx/"
    cd - > /dev/null
    echo "libvpx: $(ls -la "$OUT/lib/libvpx.a" | awk '{print $5}') bytes"
fi
file "$OUT/lib/"*.a 2>/dev/null | head -2 || true

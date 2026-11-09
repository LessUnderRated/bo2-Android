#!/usr/bin/env bash
# scripts/test-arm64.sh — the tests that need an arm64 CPU, built with the NDK as static executables.
#
# Runs them under qemu-aarch64 (Linux or WSL: apt install qemu-user openssl), or on a phone with --adb. Today:
# runtime/tests/platform_android_test.cpp, bo2-mac's Android platform layer (coroutine first entry, thread pointer,
# semaphores, 3DES checked against OpenSSL).
set -euo pipefail
cd "$(dirname "$0")/.."
UP=android/build/upstream
[ -d "$UP/bo2-mac" ] || scripts/upstream.sh
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/$(sed -n 's/^ndk;//p' android/packages.txt)}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.exe ;; *) HOST=linux-x86_64; EXE= ;; esac
CXX="$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang++$EXE"
OUT=android/build/arm64-tests
mkdir -p "$OUT"
"$CXX" --target=aarch64-linux-android33 -static -std=c++17 -O1 -g -Wall -Wextra -I "$UP/bo2-mac/runtime/win32" \
    runtime/tests/platform_android_test.cpp "$UP/bo2-mac/runtime/win32/bo2_platform_android.cpp" \
    -o "$OUT/platform_android_test"

# The 3DES-CBC reference comes from OpenSSL, not from this code.
K=0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123
P=54686520717566636B2062726F776E20666F78206A756D70
REF=""
if command -v openssl > /dev/null && command -v xxd > /dev/null; then
    REF=$(printf '%s' $P | xxd -r -p | openssl enc -des-ede3-cbc -nopad -K $K -iv 1122334455667788 | od -An -tx1 | tr -d ' \n')
fi

if [ "${1:-}" = "--adb" ]; then
    adb push "$OUT/platform_android_test" /data/local/tmp/ > /dev/null
    adb shell "BO2_TEST_DES3_CBC=$REF /data/local/tmp/platform_android_test"
else
    [ -n "$REF" ] || echo "no openssl/xxd: the multi-block 3DES-CBC value is not cross-checked"
    if command -v qemu-aarch64 > /dev/null; then
        BO2_TEST_DES3_CBC=$REF qemu-aarch64 "$OUT/platform_android_test"
    elif [ "$HOST" = windows-x86_64 ] && command -v wsl.exe > /dev/null; then
        # Git Bash on Windows: built with the Windows NDK above, run by WSL's qemu-aarch64.
        bin=$(wsl.exe wslpath -a "$(cygpath -m "$PWD/$OUT/platform_android_test")" | tr -d '\r')
        MSYS_NO_PATHCONV=1 wsl.exe env BO2_TEST_DES3_CBC="$REF" qemu-aarch64 "$bin"
    else
        echo "no qemu-aarch64: apt install qemu-user (Linux or WSL), or run with --adb" >&2
        exit 1
    fi
fi

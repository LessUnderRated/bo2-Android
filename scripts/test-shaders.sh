#!/usr/bin/env bash
# scripts/test-shaders.sh — the DXBC -> Vulkan GLSL -> SPIR-V path (runtime/gfx/dxbc_to_vk.cpp), on the PC.
#
# Builds HLSLcc (bo2-mac's vendored copy, from scripts/upstream.sh) and the translator for this machine, runs
# runtime/tests/shader_vk_test.cpp on our own test shaders, then compiles each output with the NDK's glslc for
# Vulkan 1.1 and checks it with spirv-val. dynindex_ps is HLSLcc's known gap and must FAIL to compile (detected, not
# hidden). Needs a host C++ compiler (CXX, default c++; on Windows, LLVM's clang++) and the NDK.
set -euo pipefail
cd "$(dirname "$0")/.."
UP=android/build/upstream
[ -d "$UP/bo2-mac" ] || scripts/upstream.sh
H="$UP/bo2-mac/third_party/HLSLcc"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/$(sed -n 's/^ndk;//p' android/packages.txt)}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.exe ;; *) HOST=linux-x86_64; EXE= ;; esac
TOOLS="$NDK/shader-tools/$HOST"
CXX="${CXX:-c++}"
CC="${CC:-cc}"
OUT=android/build/shader-tests
mkdir -p "$OUT/obj"
rm -rf "$OUT/glsl" && mkdir -p "$OUT/glsl"

if [ ! -f "$OUT/obj/.built" ]; then
    flags=(-O1 -w -D_CRT_SECURE_NO_WARNINGS -I "$H" -I "$H/include" -I "$H/src" -I "$H/src/internal_includes" -I "$H/src/cbstring")
    pids=()
    for f in "$H"/src/*.cpp; do "$CXX" -c -std=c++14 "${flags[@]}" "$f" -o "$OUT/obj/$(basename "$f" .cpp).o" & pids+=($!); done
    for f in "$H"/src/cbstring/*.c; do "$CC" -c "${flags[@]}" "$f" -o "$OUT/obj/$(basename "$f" .c).o" & pids+=($!); done
    for p in "${pids[@]}"; do wait "$p"; done
    touch "$OUT/obj/.built"
fi
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_CRT_SECURE_NO_WARNINGS -I "$H/include" \
    runtime/tests/shader_vk_test.cpp runtime/gfx/dxbc_to_vk.cpp "$OUT"/obj/*.o -o "$OUT/shader_vk_test$EXE"
"$OUT/shader_vk_test$EXE" runtime/tests/shaders "$OUT/glsl"

fail=0
for f in "$OUT"/glsl/*.vert "$OUT"/glsl/*.frag; do
    n=$(basename "$f")
    if "$TOOLS/glslc$EXE" --target-env=vulkan1.1 "$f" -o "$OUT/glsl/$n.spv" 2> "$OUT/glsl/$n.log" &&
       "$TOOLS/spirv-val$EXE" --target-env vulkan1.1 "$OUT/glsl/$n.spv" 2>> "$OUT/glsl/$n.log"; then
        result=ok
    else
        result=error
    fi
    case "$n:$result" in
        dynindex_ps.*:error) echo "  $n: rejected by glslc, as expected (HLSLcc's dynamic cbuffer indexing gap)" ;;
        dynindex_ps.*:ok)    echo "  $n: compiled: HLSLcc's gap is gone, update this test"; fail=1 ;;
        *:ok)                echo "  $n: SPIR-V valid" ;;
        *)                   echo "  $n: FAILED"; sed 's/^/    /' "$OUT/glsl/$n.log" | head -8; fail=1 ;;
    esac
done
[ "$fail" = 0 ] && echo "test-shaders: all passed" || { echo "test-shaders: FAILED"; exit 1; }

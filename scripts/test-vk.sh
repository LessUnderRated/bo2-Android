#!/usr/bin/env bash
# scripts/test-vk.sh — the Vulkan backend (runtime/gfx/vk) on the PC, headless, driven as the runtime drives it.
#
# Builds glslang (the NDK's copy, as on the phone), HLSLcc, dxbc_to_vk.cpp, the backend and
# runtime/tests/vk_backend_test.cpp for this machine, then runs the test twice: as the GPU is, and with
# BO2_VK_NO_BC=1 (BC textures decoded on the CPU, the path for GPUs without BC). Needs a Vulkan 1.3 GPU and driver,
# a host C++ compiler (CXX/CC; on Windows, LLVM's clang++/clang) and the NDK (headers, glslang sources).
set -euo pipefail
cd "$(dirname "$0")/.."
UP=android/build/upstream
[ -d "$UP/bo2-mac" ] || scripts/upstream.sh
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/$(sed -n 's/^ndk;//p' android/packages.txt)}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.exe; WIN=1 ;; *) HOST=linux-x86_64; EXE=; WIN=0 ;; esac
# HOST_ARCH=aarch64 with CXX/CC=aarch64-linux-gnu-g++/gcc: an arm64 Linux build, run under qemu-user (binfmt) with
# Mesa's lavapipe for arm64: the device the game core is built for, on the PC (scripts/harness/README.md).
[ -n "${HOST_ARCH:-}" ] && HOST=linux-$HOST_ARCH
CXX="${CXX:-c++}"
CC="${CC:-cc}"
OUT=android/build/vk-test-$HOST
G="$NDK/sources/third_party/shaderc/third_party/glslang"
H="$UP/bo2-mac/third_party/HLSLcc"
mkdir -p "$OUT/include/glslang" "$OUT/obj/glslang" "$OUT/obj/hlslcc" "$OUT/obj/vk"

# Vulkan headers from the NDK (only vulkan/ and vk_video/: the rest of the sysroot is bionic's).
SYS=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/sysroot/usr/include | head -1)   # headers only: any host's copy
cp -r "$SYS/vulkan" "$SYS/vk_video" "$OUT/include/"
# glslang's build_info.h (its build generates it from CHANGES.md: 11.11.0).
cat > "$OUT/include/glslang/build_info.h" <<'EOF'
#ifndef GLSLANG_BUILD_INFO
#define GLSLANG_BUILD_INFO
#define GLSLANG_VERSION_MAJOR 11
#define GLSLANG_VERSION_MINOR 11
#define GLSLANG_VERSION_PATCH 0
#define GLSLANG_VERSION_FLAVOR ""
#define GLSLANG_VERSION_GREATER_THAN(major, minor, patch) \
    ((GLSLANG_VERSION_MAJOR) > (major) || ((major) == GLSLANG_VERSION_MAJOR && \
    ((GLSLANG_VERSION_MINOR) > (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH) > (patch)))))
#define GLSLANG_VERSION_GREATER_OR_EQUAL_TO(major, minor, patch) \
    ((GLSLANG_VERSION_MAJOR) > (major) || ((major) == GLSLANG_VERSION_MAJOR && \
    ((GLSLANG_VERSION_MINOR) > (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH >= (patch))))))
#define GLSLANG_VERSION_LESS_THAN(major, minor, patch) \
    ((GLSLANG_VERSION_MAJOR) < (major) || ((major) == GLSLANG_VERSION_MAJOR && \
    ((GLSLANG_VERSION_MINOR) < (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH) < (patch)))))
#define GLSLANG_VERSION_LESS_OR_EQUAL_TO(major, minor, patch) \
    ((GLSLANG_VERSION_MAJOR) < (major) || ((major) == GLSLANG_VERSION_MAJOR && \
    ((GLSLANG_VERSION_MINOR) < (minor) || ((minor) == GLSLANG_VERSION_MINOR && (GLSLANG_VERSION_PATCH <= (patch))))))
#endif
EOF

# glslang: the sources the NDK's Android.mk builds, without the HLSL front end (not used).
GSRC="glslang/GenericCodeGen/CodeGen.cpp glslang/GenericCodeGen/Link.cpp OGLCompilersDLL/InitializeDll.cpp
      SPIRV/GlslangToSpv.cpp SPIRV/InReadableOrder.cpp SPIRV/Logger.cpp SPIRV/SpvBuilder.cpp SPIRV/SpvPostProcess.cpp
      SPIRV/doc.cpp SPIRV/disassemble.cpp SPIRV/SpvTools.cpp"
for f in attribute Constant glslang_tab InfoSink Initialize Intermediate intermOut IntermTraverse iomapper limits \
         linkValidate parseConst ParseContextBase ParseHelper PoolAlloc propagateNoContraction reflection RemoveTree \
         Scan ShaderLang SpirvIntrinsics SymbolTable Versions; do GSRC="$GSRC glslang/MachineIndependent/$f.cpp"; done
for f in PpAtom PpContext Pp PpScanner PpTokens; do GSRC="$GSRC glslang/MachineIndependent/preprocessor/$f.cpp"; done
[ "$WIN" = 1 ] || GSRC="$GSRC glslang/OSDependent/Unix/ossource.cpp"
GFLAGS=(-std=c++17 -O1 -w -include cstdint -D_CRT_SECURE_NO_WARNINGS -I "$G" -I "$G/glslang/MachineIndependent" -I "$OUT/include")
[ "$WIN" = 1 ] || GFLAGS+=(-DGLSLANG_OSINCLUDE_UNIX)
pids=()
if [ ! -f "$OUT/obj/glslang/.built" ]; then
    for f in $GSRC; do "$CXX" -c "${GFLAGS[@]}" "$G/$f" -o "$OUT/obj/glslang/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')" & pids+=($!); done
fi
if [ ! -f "$OUT/obj/hlslcc/.built" ]; then
    HF=(-O1 -w -include stdint.h -D_CRT_SECURE_NO_WARNINGS -I "$H" -I "$H/include" -I "$H/src" -I "$H/src/internal_includes" -I "$H/src/cbstring")
    for f in "$H"/src/*.cpp; do "$CXX" -c -std=c++14 "${HF[@]}" "$f" -o "$OUT/obj/hlslcc/$(basename "$f" .cpp).o" & pids+=($!); done
    for f in "$H"/src/cbstring/*.c; do "$CC" -c "${HF[@]}" "$f" -o "$OUT/obj/hlslcc/$(basename "$f" .c).o" & pids+=($!); done
fi
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "test-vk.sh: glslang or HLSLcc did not build" >&2; exit 1; }
touch "$OUT/obj/glslang/.built" "$OUT/obj/hlslcc/.built"

# The backend, the translator, the test.
VFLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -D_CRT_SECURE_NO_WARNINGS
        -I runtime/gfx/vk -I "$UP/bo2-mac/runtime/win32" -I "$H/include" -I "$G" -I "$OUT/include")
pids=()
SRCS="runtime/gfx/vk/vk_core.cpp runtime/gfx/vk/vk_capture.cpp runtime/gfx/vk/vk_resources.cpp runtime/gfx/vk/vk_render.cpp
      runtime/gfx/vk/vk_frame.cpp runtime/gfx/vk/vk_window.cpp runtime/gfx/vk/vk_glsl.cpp runtime/gfx/vk/bc_decode.cpp
      runtime/gfx/dxbc_to_vk.cpp runtime/tests/vk_backend_test.cpp"
[ "$WIN" = 1 ] && SRCS="$SRCS runtime/tests/glslang_os_win.cpp"
for f in $SRCS; do "$CXX" -c "${VFLAGS[@]}" "$f" -o "$OUT/obj/vk/$(basename "$f" .cpp).o" 2> "$OUT/obj/vk/$(basename "$f" .cpp).log" & pids+=("$!:$f"); done
for pf in "${pids[@]}"; do
    if ! wait "${pf%%:*}"; then echo "  FAIL ${pf#*:}"; grep -m8 'error' "$OUT/obj/vk/$(basename "${pf#*:}" .cpp).log"; fail=1; fi
done
[ "$fail" = 0 ] || exit 1
LIBS=()
[ "$WIN" = 1 ] || LIBS=(-ldl -lpthread)
"$CXX" -g "$OUT"/obj/vk/*.o "$OUT"/obj/glslang/*.o "$OUT"/obj/hlslcc/*.o "${LIBS[@]}" -o "$OUT/vk_backend_test$EXE"

# VALIDATE=1: under the Khronos validation layer with synchronization validation; any error or hazard fails the run.
run() {
    if [ "${VALIDATE:-0}" = 1 ]; then
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 "$@" > "$OUT/run.log" 2>&1 || { cat "$OUT/run.log"; return 1; }
        grep -E "scene|passed|FAIL" "$OUT/run.log"
        local n
        n=$(grep -c -E "Validation (Error|Warning)" "$OUT/run.log" || true)
        echo "  validation messages: $n"
        [ "$n" = 0 ] || { grep -A4 -E "Validation (Error|Warning)" "$OUT/run.log" | head -60; return 1; }
    else
        "$@"
    fi
}
echo "== the GPU as it is"
run "$OUT/vk_backend_test$EXE" runtime/tests/shaders
echo "== BO2_VK_NO_BC=1 (BC decoded on the CPU)"
BO2_VK_NO_BC=1 run "$OUT/vk_backend_test$EXE" runtime/tests/shaders

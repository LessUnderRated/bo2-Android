#!/usr/bin/env bash
# scripts/package-game.sh — the game core as one static library for the APK (bo2.gameCore), on Linux or WSL.
#
#   scripts/package-game.sh <bo2-mac checkout, lifted> <android NDK r28c for Linux> <deps dir> <out dir>
#
# Takes scripts/lift-android.sh's <checkout>/boot_cache/zms/android/ and builds, for aarch64-linux-android33:
#   bo2-mac's portable runtime (with the patches and the player's generated reverb body), this port's Vulkan backend
#   and shader translator, HLSLcc, glslang, the generated entry (bo2_game_main) and interpose stubs, the redirects,
#   the lifted code, Unicorn and libvpx  ->  <out>/libbo2game.a
# and copies pe_sections.bin to <out>/ for the phone's game folder. All of it is derived from the player's own exe and
# stays on their machine: it is for their phone only and may not be shared.
#
# What the macOS link does by patching its binary is done here with ELF tools, before the link:
#   redirects      a lifted function the runtime replaces by a native body (memcpy, longjmp, skinning, ...): the
#                  lifted definition is made weak and a strong trampoline sub_<va> -> bo2_<native> is added.
#   substitutions  a sub_<va> the runtime defines for another executable while this lift defines it too: the
#                  runtime's definition is made local, so the lift wins (unless the target wants it: shim_defined).
set -euo pipefail
BO2="${1:?usage}"; NDK="${2:?usage}"; DEPS="${3:?usage}"; OUT="${4:?usage}"
APK="$(cd "$(dirname "$0")/.." && pwd)"
A="$BO2/boot_cache/zms/android"
for f in main.cpp interpose_stubs.cpp lifted.o pe_sections.bin redirects.txt shim_defined.txt; do
    [ -e "$A/$f" ] || { echo "package-game.sh: $A/$f missing: run scripts/lift-android.sh first" >&2; exit 1; }
done
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX="$TC/aarch64-linux-android33-clang++"; CC="$TC/aarch64-linux-android33-clang"
NM=$TC/llvm-nm; OBJCOPY=$TC/llvm-objcopy; AR=$TC/llvm-ar
# HARNESS=1: the same core for arm64 Linux (glibc), linked into runtime/harness's test executable, which runs on the
# PC under qemu-user (scripts/harness/). Unicorn and libvpx come from Ubuntu's arm64 packages (headers included).
HARNESS="${HARNESS:-0}"
XFLAGS=()
if [ "$HARNESS" = 1 ]; then
    CXX=aarch64-linux-gnu-g++; CC=aarch64-linux-gnu-gcc
    # No _FORTIFY_SOURCE (Ubuntu's GCC turns it on): the runtime longjmps between its coroutine stacks on purpose,
    # and glibc's checked longjmp aborts that as "longjmp causes uninitialized stack frame". Bionic does not check.
    XFLAGS=(-DBO2_HARNESS -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I "$APK/runtime/harness/include")
    DEPS=${HARNESS_DEPS:-$HOME/bo2-harness-deps}
    mkdir -p "$DEPS/include"
    ln -sfn /usr/include/unicorn "$DEPS/include/unicorn"; ln -sfn /usr/include/vpx "$DEPS/include/vpx"
fi
WORK=${PACKAGE_WORK:-$HOME/bo2-package}
rm -rf "$WORK" && mkdir -p "$WORK"/{rt,vk,hlslcc,glslang,gen} "$OUT"
J=$(nproc)
R="$BO2/runtime"
H="$BO2/third_party/HLSLcc"
G="$NDK/sources/third_party/shaderc/third_party/glslang"
REM="$BO2/tools/remill-src/include"
say(){ echo "[package $(date +%H:%M:%S)] $*"; }

# ---- Redirects: bo2-mac's list, plus the Zombies mixer reverb ----------------------------------------------------------
# lift-android.sh's reverb stage translates this exe's reverb (0x83c170-0x83cd94, x86c.py) into bo2_snd_reverb_zm, but
# boot.py's list never sent the lifted function there: the lifted reverb ran on an audio thread, 14.6% of it (S26).
mkdir -p "$WORK/gen"
RD="$WORK/gen/redirects.txt"
cp "$A/redirects.txt" "$RD"
if [ "$HARNESS" != 1 ] && grep -q 'bo2_snd_reverb_zm' "$R/win32/snd_reverb_gen.inc" 2>/dev/null && ! grep -q '^sub_83c170 ' "$RD"; then
    echo "sub_83c170 bo2_snd_reverb_zm" >> "$RD"
fi

# ---- Compile ---------------------------------------------------------------------------------------------------------
pids=()
run(){ "$@" & pids+=($!); while [ "$(jobs -rp | wc -l)" -ge "$J" ]; do wait -n || true; done; }
# -march armv8.2-a: LSE atomics inline (casal, ldadd) instead of the outline __aarch64_cas* helpers, which every
# guest `lock` instruction went through (the runtime's lock and counters). Every phone the port runs on has them.
RTFLAGS=(-c -O1 -std=c++17 -fPIC -w -march=armv8.2-a -DADDRESS_SIZE_BITS=32 -DBO2_NO_EXTRA_LIFTED -DBO2_TARGET_ZMS "${XFLAGS[@]}"
         -I "$REM" -I "$R/win32" -I "$H/include" -I "$DEPS/include" -I "$BO2/boot_cache/zms/inc_override")
say "runtime"
for f in win32/win32_runtime.cpp win32/kernel32_impl.cpp win32/user32_impl.cpp win32/remaining_dlls_impl.cpp \
         win32/bo2_replay.cpp win32/heap_code.cpp win32/x86_guest_vm.cpp win32/interp.cpp win32/exception_adapter.cpp \
         win32/dw_local.cpp win32/cull_native.cpp win32/skin_native.cpp win32/match_rules.cpp video/vpx_native.cpp \
         audio/coreaudio_out.cpp win32/bo2_platform_android.cpp; do
    run "$CXX" "${RTFLAGS[@]}" "$R/$f" -o "$WORK/rt/$(basename "$f" .cpp).o"
done
# The renderer and the shader translator are the APK's (android/native/CMakeLists.txt builds them from source); only
# the PC test harness (HARNESS=1), a single executable, still compiles them here.
if [ "$HARNESS" = 1 ]; then
say "Vulkan backend, translator"
# The translation cache is keyed on what decides the translation (dxbc_to_vk.cpp and HLSLcc), not on the build time:
# a new APK with the same translator keeps the phone's cache.
TRV=$({ cat "$APK/runtime/gfx/dxbc_to_vk.cpp"; find "$H/src" "$H/include" -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' \) \
      | LC_ALL=C sort | xargs cat; } | sha1sum | cut -c1-12)
say "translator version $TRV"
VKFLAGS=(-c -O2 -std=c++17 -fPIC -w -DBO2_TR_VERSION="\"$TRV\"" -I "$APK/runtime/gfx/vk" -I "$R/win32" -I "$H/include" -I "$G")
for f in "$APK"/runtime/gfx/vk/*.cpp "$APK"/runtime/gfx/dxbc_to_vk.cpp; do
    run "$CXX" "${VKFLAGS[@]}" "$f" -o "$WORK/vk/$(basename "$f" .cpp).o"
done
say "HLSLcc, glslang"
HF=(-c -O2 -fPIC -w -I "$H" -I "$H/include" -I "$H/src" -I "$H/src/internal_includes" -I "$H/src/cbstring")
for f in "$H"/src/*.cpp; do run "$CXX" -std=c++14 "${HF[@]}" "$f" -o "$WORK/hlslcc/$(basename "$f" .cpp).o"; done
for f in "$H"/src/cbstring/*.c; do run "$CC" "${HF[@]}" "$f" -o "$WORK/hlslcc/$(basename "$f" .c).o"; done
mkdir -p "$WORK/gen/glslang"
{ printf '#ifndef GLSLANG_BUILD_INFO\n#define GLSLANG_BUILD_INFO\n#define GLSLANG_VERSION_MAJOR 11\n#define GLSLANG_VERSION_MINOR 11\n'
  printf '#define GLSLANG_VERSION_PATCH 0\n#define GLSLANG_VERSION_FLAVOR ""\n'
  sed -n '/^#define GLSLANG_VERSION_GREATER_THAN/,$p' "$G/build_info.h.tmpl"; } > "$WORK/gen/glslang/build_info.h"
GSRC="glslang/GenericCodeGen/CodeGen.cpp glslang/GenericCodeGen/Link.cpp OGLCompilersDLL/InitializeDll.cpp
      glslang/OSDependent/Unix/ossource.cpp SPIRV/GlslangToSpv.cpp SPIRV/InReadableOrder.cpp SPIRV/Logger.cpp
      SPIRV/SpvBuilder.cpp SPIRV/SpvPostProcess.cpp SPIRV/doc.cpp SPIRV/disassemble.cpp SPIRV/SpvTools.cpp"
for f in attribute Constant glslang_tab InfoSink Initialize Intermediate intermOut IntermTraverse iomapper limits \
         linkValidate parseConst ParseContextBase ParseHelper PoolAlloc propagateNoContraction reflection RemoveTree \
         Scan ShaderLang SpirvIntrinsics SymbolTable Versions; do GSRC="$GSRC glslang/MachineIndependent/$f.cpp"; done
for f in PpAtom PpContext Pp PpScanner PpTokens; do GSRC="$GSRC glslang/MachineIndependent/preprocessor/$f.cpp"; done
for f in $GSRC; do
    run "$CXX" -c -O2 -fPIC -w -std=c++17 -include cstdint -DGLSLANG_OSINCLUDE_UNIX -I "$G" -I "$G/glslang/MachineIndependent" \
        -I "$WORK/gen" "$G/$f" -o "$WORK/glslang/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
done
fi
say "entry, stubs, redirects"
{
    echo '// GENERATED by scripts/package-game.sh: lifted functions the runtime replaces by native bodies.'
    echo '#include <cstdint>'
    echo 'struct State;'
    while read -r sym native; do
        echo "extern \"C\" void* $native(State*, uint32_t, void*);"
        echo "extern \"C\" __attribute__((used)) void* $sym(State* s, uint32_t pc, void* m) { return $native(s, pc, m); }"
    done < "$RD"
} > "$WORK/gen/redirects.cpp"
for f in main interpose_stubs; do
    run "$CXX" -c -O1 -std=c++17 -fPIC -w -include cstdio -include cstdlib -include cstring -include vector -include signal.h \
        -include pthread.h -include ctime -I "$REM" -I "$R/win32" "$A/$f.cpp" -o "$WORK/gen/$f.o"
done
run "$CXX" -c -O2 -std=c++17 -fPIC -w "$WORK/gen/redirects.cpp" -o "$WORK/gen/redirects.o"
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "package-game.sh: a compile failed" >&2; exit 1; }

# ---- The macOS link's patching, with ELF tools ------------------------------------------------------------------------
cp "$A/lifted.o" "$WORK/lifted.o"
say "redirects: $(wc -l < "$RD") lifted functions made weak"
args=()
while read -r sym native; do args+=(--weaken-symbol="$sym"); done < "$RD"
[ ${#args[@]} -eq 0 ] || "$OBJCOPY" "${args[@]}" "$WORK/lifted.o"
# The lifter's x87 semantics (fsqrt, fpatan, fptan, fyl2x, f2xm1, fprem, fprem1) call the long double libm functions
# with a double in and a double out: Remill's semantics bitcode was built where long double is a double (macOS
# arm64). Android's (and glibc's) arm64 long double is a 128-bit quad: sqrtl read q0 and answered in q0, so the game
# got garbage (Lua math.sqrt(10961) = -inf; the zombies globe's -inf longitude then spun its angle wrap forever at
# SOLO PLAY). The calls go to double versions of the same functions (runtime/x87_libm.cpp).
X87_LIBM="sqrtl atanl tanl log2l exp2l fmodl remainderl"
"$OBJCOPY" $(for f in $X87_LIBM; do printf -- '--redefine-sym %s=bo2_x87_%s ' "$f" "$f"; done) "$WORK/lifted.o"
"$CXX" -c -O2 -std=c++17 -fPIC "$APK/runtime/x87_libm.cpp" -o "$WORK/gen/x87_libm.o"
left=$("$NM" -u "$WORK/lifted.o" | awk '{print $NF}' |
    grep -E '^(a?(sin|cos|tan)h?|atan2|exp2?|expm1|log(2|10|1p)?|pow|sqrt|cbrt|hypot|fmod|remainder|remquo|ldexp|frexp|modf|scalbn|ilogb|logb|floor|ceil|trunc|l?l?round|l?l?rint|nearbyint|fabs|fma|fmin|fmax|fdim|copysign|nextafter|erfc?|[lt]gamma)l$' || true)
[ -z "$left" ] || say "warning: lifted.o still calls long double functions: $left"
"$NM" --defined-only --extern-only "$WORK/lifted.o" | awk '{print $NF}' | grep '^sub_' | sort -u > "$WORK/lifted.syms"
sort -u "$A/shim_defined.txt" > "$WORK/wanted.syms"
nloc=0
for o in "$WORK"/rt/*.o; do
    "$NM" --defined-only --extern-only "$o" | awk '{print $NF}' | grep '^sub_' | sort -u > "$o.syms" || true
    comm -12 "$o.syms" "$WORK/lifted.syms" | comm -23 - "$WORK/wanted.syms" > "$o.loc"
    if [ -s "$o.loc" ]; then
        "$OBJCOPY" $(sed 's/^/--localize-symbol=/' "$o.loc") "$o"
        nloc=$((nloc + $(wc -l < "$o.loc")))
    fi
done
say "substitutions for other executables made local in the runtime: $nloc"

# The interpose stubs (sub_<va>_impl bodies of another executable, abort-only) give way to any real definition:
# native_impl.sh joins this executable's own _impl bodies to lifted.o.
"$OBJCOPY" $("$NM" --defined-only --extern-only "$WORK/gen/interpose_stubs.o" | awk '{print "--weaken-symbol=" $NF}') \
    "$WORK/gen/interpose_stubs.o"

# Debug probes (PROBES=1 only): runtime/probes/*.cpp, each naming the lifted function it wraps on its first line
# ("// wraps: sub_<va>"). The lifted definition is made weak and gets a second name, sub_<va>_lifted, on the same code
# (offset 0 of its own section .text.sub_<va>); the probe defines sub_<va> strongly, logs, and calls sub_<va>_lifted.
# Every caller keeps naming sub_<va>, so direct calls from lifted code reach the probe too (renaming the symbol instead
# renamed those references with it, and only calls through the dispatch table were seen).
# Fixes (always, APK and harness): runtime/fixes/*.cpp, wrapped the same way. Each is a change to the game's behaviour
# on this port, with its reason in its header.
FIXED=" "
for p in "$APK"/runtime/fixes/*.cpp; do
    [ -e "$p" ] || continue
    sym=$(sed -n '1s|^// wraps: \(sub_[0-9a-f]*\)$|\1|p' "$p")
    [ -n "$sym" ] || { echo "package-game.sh: $p has no '// wraps: sub_<va>' first line" >&2; exit 1; }
    grep -qx "$sym" "$WORK/lifted.syms" || { echo "package-game.sh: $sym is not in lifted.o" >&2; exit 1; }
    "$OBJCOPY" --weaken-symbol="$sym" --add-symbol "${sym}_lifted=.text.$sym:0,global,function" "$WORK/lifted.o"
    "$CXX" -c -O1 -std=c++17 -fPIC -w -I "$REM" "$p" -o "$WORK/gen/fix_$(basename "$p" .cpp).o"
    FIXED="$FIXED$sym "
    say "fix $(basename "$p"): $sym wrapped"
done
# PROBE_COUNT="sub_<va> ..." (with PROBES=1): generated counting probes, a line on the 1st and every 300th call with the
# caller, to see which link of a call chain stops running.
if [ "${PROBES:-0}" = 1 ] && [ -n "${PROBE_COUNT:-}" ]; then
    mkdir -p "$WORK/gen/count"
    for sym in $PROBE_COUNT; do
        f="$WORK/gen/count/count_$sym.cpp"
        { echo "// wraps: $sym"
          echo '#include "remill/Arch/X86/Runtime/State.h"'
          echo '#include <cstdint>'; echo '#include <cstdio>'
          echo 'extern "C" uint32_t win32_mem_read32(uint32_t va);'
          echo "extern \"C\" void* ${sym}_lifted(State*, uint32_t, void*);"
          echo "extern \"C\" void* $sym(State* s, uint32_t pc, void* mem) {"
          echo '    static unsigned n = 0;'
          echo "    if ((++n % 300) == 1) fprintf(stderr, \"[COUNT] $sym #%u (from 0x%08x)\\n\", n, win32_mem_read32(s->gpr.rsp.dword));"
          echo "    return ${sym}_lifted(s, pc, mem);"
          echo '}'; } > "$f"
    done
fi
# PROBE_CHAIN="sub_<va> ..." (with PROBES=1): generated call-chain probes, the guest call chain (code addresses on the
# stack) on the 1st and every 200th call, to see where a thread is when it keeps calling a function.
if [ "${PROBES:-0}" = 1 ] && [ -n "${PROBE_CHAIN:-}" ]; then
    mkdir -p "$WORK/gen/count"
    for sym in $PROBE_CHAIN; do
        f="$WORK/gen/count/chain_$sym.cpp"
        { echo "// wraps: $sym"
          echo '#include "remill/Arch/X86/Runtime/State.h"'
          echo '#include <cstdint>'; echo '#include <cstdio>'
          echo 'extern "C" uint32_t win32_mem_read32(uint32_t va);'
          echo 'extern "C" int win32_current_tid(void);'
          echo "extern \"C\" void* ${sym}_lifted(State*, uint32_t, void*);"
          echo "extern \"C\" void* $sym(State* s, uint32_t pc, void* mem) {"
          echo '    static unsigned n = 0;'
          echo '    if ((++n % 200) == 1) {'
          echo '        const uint32_t esp = s->gpr.rsp.dword; char c[700]; int k = 0;'
          echo '        for (uint32_t i = 0; i < 200 && k < 680; i++) { const uint32_t v = win32_mem_read32(esp + 4 * i);'
          echo '            if (v >= 0x401000 && v < 0xb00000) k += snprintf(c + k, sizeof c - (size_t)k, " %x", v); }'
          echo '        c[k] = 0;'
          echo "        fprintf(stderr, \"[CHAIN] $sym #%u tid %d:%s\\n\", n, win32_current_tid(), c);"
          echo '    }'
          echo "    return ${sym}_lifted(s, pc, mem);"
          echo '}'; } > "$f"
    done
fi
if [ "${PROBES:-0}" = 1 ]; then
    for p in "$APK"/runtime/probes/*.cpp ${PROBE_COUNT:+$WORK/gen/count/count_*.cpp} ${PROBE_CHAIN:+$WORK/gen/count/chain_*.cpp}; do
        case "$FIXED" in *" $(sed -n '1s|^// wraps: \(sub_[0-9a-f]*\)$|\1|p' "$p") "*)
            echo "package-game.sh: $p wraps a function a fix already wraps" >&2; exit 1 ;; esac
        sym=$(sed -n '1s|^// wraps: \(sub_[0-9a-f]*\)$|\1|p' "$p")
        [ -n "$sym" ] || { echo "package-game.sh: $p has no '// wraps: sub_<va>' first line" >&2; exit 1; }
        grep -qx "$sym" "$WORK/lifted.syms" || { echo "package-game.sh: $sym is not in lifted.o" >&2; exit 1; }
        "$OBJCOPY" --weaken-symbol="$sym" --add-symbol "${sym}_lifted=.text.$sym:0,global,function" "$WORK/lifted.o"
        "$CXX" -c -O1 -std=c++17 -fPIC -w -I "$REM" "$p" -o "$WORK/gen/probe_$(basename "$p" .cpp).o"
        say "probe $(basename "$p"): $sym wrapped"
    done
fi

# boot.py's second link pass: interposes of other executables also reach sub_<va>_impl<N> names made by macros
# (BO2_ESP_AUDIT) and bare sub_<va>, which the source scan above misses. Everything referenced and defined nowhere
# gets a body that aborts with its name, as on macOS (a lifted definition is never shadowed: it is "defined").
OBJS=("$WORK"/rt/*.o "$WORK"/gen/*.o "$WORK/lifted.o")
[ "$HARNESS" != 1 ] || OBJS+=("$WORK"/vk/*.o)
"$NM" --undefined-only "${OBJS[@]}" | awk '{print $NF}' | grep -E '^sub_[0-9a-f]+(_impl[0-9]*)?$' | sort -u > "$WORK/undef.syms"
"$NM" --defined-only "${OBJS[@]}" | awk '{print $NF}' | grep -E '^sub_[0-9a-f]+(_impl[0-9]*)?$' | sort -u > "$WORK/def.syms"
comm -23 "$WORK/undef.syms" "$WORK/def.syms" > "$WORK/missing.syms"
{
    echo '// GENERATED by scripts/package-game.sh: symbols of other executables the runtime names; fatal if ever run.'
    echo '#include <cstdio>'; echo '#include <cstdlib>'
    while read -r sym; do
        echo "extern \"C\" void* $sym(void*, unsigned, void*) {"
        echo "    std::fprintf(stderr, \"[TARGET] $sym belongs to another executable and must not run on zms\\n\");"
        echo "    std::abort();"
        echo "}"
    done < "$WORK/missing.syms"
} > "$WORK/gen/missing_stubs.cpp"
"$CXX" -c -O1 -std=c++17 -fPIC -w "$WORK/gen/missing_stubs.cpp" -o "$WORK/gen/missing_stubs.o"
say "link pass 2: $(wc -l < "$WORK/missing.syms") symbols of other executables given fatal bodies"

cp "$A/pe_sections.bin" "$OUT/pe_sections.bin"
if [ "$HARNESS" = 1 ]; then
    rm -f "$OUT/libbo2game.a"
    "$AR" qc "$OUT/libbo2game.a" "$WORK"/rt/*.o "$WORK"/vk/*.o "$WORK"/hlslcc/*.o "$WORK"/glslang/*.o "$WORK"/gen/*.o \
        "$WORK/lifted.o"
    "$TC/llvm-ranlib" "$OUT/libbo2game.a"
    # The test executable: the core whole, the harness's main and AAudio stand-in, Ubuntu's arm64 libraries.
    "$CXX" -c -O1 -std=c++17 -I "$APK/runtime/gfx/vk" "$APK/runtime/harness/harness_main.cpp" -o "$WORK/harness_main.o"
    "$CXX" -c -O1 -std=c++17 -I "$APK/runtime/harness/include" "$APK/runtime/harness/aaudio_stub.cpp" -o "$WORK/aaudio_stub.o"
    "$CXX" -o "$OUT/bo2_harness" "$WORK/harness_main.o" "$WORK/aaudio_stub.o" \
        -Wl,--whole-archive "$OUT/libbo2game.a" -Wl,--no-whole-archive \
        -lunicorn -lvpx -lvulkan -lz -lpthread -ldl -lm
    say "harness: $OUT/bo2_harness ($(du -h "$OUT/bo2_harness" | cut -f1))"
else
    # ---- The game core: one shared library, opened by the app at run time (android/native/src/core_loader.cpp) ----
    # It exports only the entry points below (the version script): everything else binds inside it, so the lifted
    # code's direct references to the runtime (its dso_local globals, its calls) stay as fast as in one library. It
    # names libmain.so as a dependency (an empty stand-in of that name here), so its calls into the renderer, the pad
    # bridge and the performance hints bind to the app's library when the app opens it; and libc++_shared.so, the
    # app's, so the process has one C++ runtime.
    {
        echo '// GENERATED by scripts/package-game.sh: what the app asks the core for, besides its own entry points.'
        echo '#include <cstdint>'
        echo 'extern "C" uint32_t gfx_draw_record_size(void);'
        echo 'extern "C" float gfx_depth_clear_value();'
        echo 'extern "C" uint32_t bo2core_draw_record_size(void) { return gfx_draw_record_size(); }'
        echo 'extern "C" float bo2core_depth_clear_value(void) { return gfx_depth_clear_value(); }'
    } > "$WORK/core_api.cpp"
    "$CXX" -c -O2 -std=c++17 -fPIC -w "$WORK/core_api.cpp" -o "$WORK/core_api.o"
    {
        echo '{ global:'
        for s in bo2_game_main bo2_cmd_queue bo2_dvar_unlock_lod bo2_map_cull_distance bo2_hw_atomic_report \
                 bo2core_draw_record_size bo2core_depth_clear_value bo2_aim_state; do echo "    $s;"; done
        echo '  local: *; };'
    } > "$WORK/core.map"
    mkdir -p "$WORK/stub"
    echo '' | "$CC" -x c -shared -fPIC -Wl,-soname,libmain.so -o "$WORK/stub/libmain.so" -
    rm -f "$OUT/libbo2game.a" "$OUT/libunicorn.a" "$OUT/libvpx.a"
    say "link the game core"
    "$CXX" -shared -fPIC -o "$OUT/libbo2core.so" -Wl,-soname,libbo2core.so -Wl,--version-script="$WORK/core.map" \
        -Wl,-Bsymbolic -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384 \
        "$WORK"/rt/*.o "$WORK"/gen/*.o "$WORK/lifted.o" "$WORK/core_api.o" \
        "$DEPS/lib/libunicorn.a" "$DEPS/lib/libvpx.a" \
        -Wl,--no-as-needed "$WORK/stub/libmain.so" -Wl,--as-needed \
        -nostdlib++ -lc++_shared -laaudio -llog -landroid -lz -lm -ldl
    # Every symbol it leaves to the loader must be the app's (libmain.so) or the system's: list the rest.
    "$NM" -D --undefined-only "$OUT/libbo2core.so" | awk '{print $NF}' | sed 's/@.*//' | sort -u > "$WORK/core.undef"
    SYS="$TC/../sysroot/usr/lib/aarch64-linux-android/33"
    for l in libc libm libdl liblog libaaudio libandroid libz; do "$NM" -D --defined-only "$SYS/$l.so" 2>/dev/null; done |
        awk '{print $NF}' | sed 's/@.*//' | sort -u > "$WORK/sys.def"
    "$NM" -D --defined-only "$TC/../sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" | awk '{print $NF}' |
        sed 's/@.*//' | sort -u >> "$WORK/sys.def"
    sort -u -o "$WORK/sys.def" "$WORK/sys.def"
    comm -23 "$WORK/core.undef" "$WORK/sys.def" > "$WORK/core.from_app"
    say "the core leaves $(wc -l < "$WORK/core.from_app") symbols to the app (libmain.so): $(tr '\n' ' ' < "$WORK/core.from_app" | cut -c1-600)"
    cp "$WORK/core.from_app" "$OUT/libbo2core.from_app.txt"
fi
# The redirects only take if callers reach sub_<va> through its symbol: count the relocations against each.
# (The APK build only: one objdump pass over lifted.o per redirect is a minute the harness cycle does not need.)
[ "$HARNESS" = 1 ] || while read -r sym native; do
    n=$("$TC/llvm-objdump" -r "$WORK/lifted.o" | grep -c "[[:space:]]$sym\$" || true)
    echo "  redirect $sym -> $native: $n call sites through the symbol"
done < "$RD"
say "done: $([ "$HARNESS" = 1 ] && echo "$OUT/libbo2game.a" || echo "$OUT/libbo2core.so ($(du -h "$OUT/libbo2core.so" | cut -f1))"), $OUT/pe_sections.bin ($(du -h "$OUT/pe_sections.bin" | cut -f1))"

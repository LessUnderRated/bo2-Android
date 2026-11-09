#!/usr/bin/env bash
# scripts/lift-android.sh — recompile the player's t6zm.exe for Android (milestone M3), on Linux or WSL.
#
#   scripts/lift-android.sh <a checkout of this repo (android branch)> [stage]
#
# bo2-mac's own pipeline (scripts/build_steam.sh), for Zombies, retargeted with BO2_ANDROID=1: the reverb body, the
# function catalogue filled from the player's exe, the lift (Remill), the -O2 fold, the native fallbacks and the
# finish step, which writes <workdir>/android/ instead of linking a macOS app. Then scripts/package-game.sh turns that
# into the static library the APK links (bo2.gameCore).
#
# Inputs: ~/.bo2-native/steam/t6zm.exe.unpacked.exe (the player's own exe, SteamStub removed by them; checked by
# bo2-mac's section hash) and ~/.bo2-native/game_dir. Everything this writes is derived from the player's exe: it
# stays on their machine (boot_cache/ is ignored by git) and is never committed or shared.
# Needs: LLVM 17 (LLVM17_PREFIX, default /usr/lib/llvm-17), python3 with capstone, cmake, ninja, zsh.
set -euo pipefail
B="${1:?usage: lift-android.sh <bo2-mac checkout> [reverb|catalogue|lift|fold|native|finish]}"
ONLY="${2:-}"
cd "$B"
export LLVM17_PREFIX="${LLVM17_PREFIX:-/usr/lib/llvm-17}" BO2_ANDROID=1 BO2_TARGET=zms
# The Rust LLVM pass in the fold (bo2-mac tools/llvm-pass: bo2-dead-flags, about 7% fewer instructions); its plugin
# is built with cargo (tools/llvm-pass/README.md). OFF by default: with it (2dc7d20..e26a468) the player caught on the
# ground in certain spots in Town, and a build folded without it (6455461-nodf) played a smooth multi-round Farm
# session. A flag store it deletes is read somewhere it does not see; until that case is found, BO2_FOLD_PASS=1 opts in.
export BO2_FOLD_PASS="${BO2_FOLD_PASS:-0}"
S=~/.bo2-native/steam; EXE=$S/t6zm.exe.unpacked.exe; N=12533; W=boot_cache/zms
C=$(nproc); M=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 2097152 )); J=$(( C < M ? C : M )); [ "$J" -ge 1 ] || J=1
F=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) * 2 / 5 / 1048576 )); F=$(( C < F ? C : F )); [ "$F" -ge 1 ] || F=1
export J
say(){ echo "[lift-android $(date +%H:%M:%S)] $*"; }
die(){ say "FAILED: $*"; exit 1; }
want(){ [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }
[ -f "$EXE" ] || die "$EXE not found (the player's unwrapped exe)"
mkdir -p boot_cache "$W"

if want remill; then
    [ -x tools/remill-src/build/bin/lift/remill-lift-17 ] || { say "Remill"; J=$J bash scripts/build_remill.sh; }
fi
if want reverb; then
    # Zombies' mixer reverb, translated by bo2-mac's x86c.py from the player's exe (same arguments as gen_snd_reverb.sh).
    if [ ! -s runtime/win32/snd_reverb_gen.inc ]; then
        say "reverb"
        python3 scripts/x86c.py "$EXE" 83c170 83cd94 bo2_snd_reverb_zm SND-REV > runtime/win32/snd_reverb_gen.inc.tmp
        mv runtime/win32/snd_reverb_gen.inc.tmp runtime/win32/snd_reverb_gen.inc
    fi
fi
if want catalogue; then
    [ -s bin/ghidra-out-zms/calltree.csv ] || { say "catalogue"; python3 tools/ghidra_csv.py remplir "$EXE"; }
fi
LIFTENV=(LIFT_PISTE_ENTIERE=1 BOOT_WORKDIR=$W LIFT_ROOTS_FILE=tools/lift_roots_zms.txt LIFT_RUNWAY_FILE=$W/runway_vide.txt)
if want lift; then
    [ -f $W/runway_vide.txt ] || : > $W/runway_vide.txt
    if [ ! -f $W/.lifted ]; then
        say "lift: $N functions, $J jobs"
        env "${LIFTENV[@]}" python3 -u tools/boot.py -n $N -j $J --build-only > $W/lift.log 2>&1 || die "lift (see $W/lift.log)"
        [ -s $W/lifted.o ] || die "lift made no lifted.o (see $W/lift.log)"
        touch $W/.lifted
    fi
fi
if want fold; then
    if [ ! -f $W/.folded ]; then
        say "fold: $F in parallel"
        BOOT_WORKDIR=$W bash scripts/fold_lift.sh $F > $W/fold.log 2>&1 || die "fold (see $W/fold.log)"
        mv ${W}_fold/lifted.o $W/lifted.o; rm -rf ${W}_fold; touch $W/.folded
        # The per-function bitcode goes; the 192 slices (batch_*.bc, ~280 MB) stay, so a change to a fold filter or
        # pass refolds them (scripts/build.sh --fold, ~5 min) without a re-lift. BO2_DROP_SLICES=1 deletes them too.
        find $W -maxdepth 1 \( -name 'func_*.bc' -o -name 'func_*.bc.fixed' \) -delete
        if [ "${BO2_DROP_SLICES:-0}" = 1 ]; then find $W -maxdepth 1 -name 'batch_*.bc' -delete; fi
    fi
fi
if want native; then
    say "native fallbacks"
    zsh tools/steam/native_impl.sh zms > $W/natif.log 2>&1 || die "native fallbacks (see $W/natif.log)"
fi
if want finish; then
    say "finish (android outputs)"
    env "${LIFTENV[@]}" BOOT_RELINK_ONLY=1 python3 -u tools/boot.py -n $N -j $J --build-only > $W/finish.log 2>&1 \
        || die "finish (see $W/finish.log)"
    [ -s $W/android/main.cpp ] && [ -s $W/android/lifted.o ] && [ -s $W/android/pe_sections.bin ] \
        || die "finish wrote no android outputs (see $W/finish.log)"
    say "done: $W/android ($(du -sh $W/android | cut -f1))"
fi

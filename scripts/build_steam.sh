#!/bin/bash
# build_steam.sh — translates the three Steam games from a fresh copy: Remill, then for campaign, Zombies and multiplayer
# the catalogue, lift, -O2 fold, native fallbacks and link. No byte of the game comes from the repository. Each step is
# skipped if its output exists, so a second run resumes. Progress: "[prepare] NN %" lines (tools/setup_window.swift).
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD; BC=boot_cache; S=~/.bo2-native/steam
# Lift: every core, efficiency ones included, and one job per 2 GB. Fold: one slice per 2.5 GB, as the biggest slices
# sit side by side (mps 122-133: 0.75 to 1.6 GB each; the top 6 together left 41 % of 16 GB free). Measured on an M1
# (8 cores, 16 GB), app toolchain: lift 4 -> 8 jobs -20 % (first 5000 sps functions); fold, 30 ordinary slices,
# 3 -> 6 in parallel -25 %, 8 -28 %. Real sps run of 11/05 (8 and 8): lift 951 -> 943 s, fold 26.5 -> 22.3 min.
C=$(sysctl -n hw.ncpu); M=$(( $(sysctl -n hw.memsize) >> 31 )); M5=$(( ($(sysctl -n hw.memsize) * 2 / 5) >> 30 ))
J=${J:-$(( C < M ? C : M ))}; [ "$J" -ge 1 ] || J=1; export J
F=${F:-$(( C < M5 ? C : M5 ))}; [ "$F" -ge 1 ] || F=1
say(){ echo "[prepare] $*"; }
die(){ say "❌ $*"; exit 1; }
libre(){ df -k "$ROOT" | awk 'NR==2{print int($4/1048576)}'; }
tue(){ local c; for c in $(pgrep -P "$1"); do tue "$c"; done; kill -9 "$1" 2>/dev/null; }
# veille <log> <command…>: runs the command, killed with its children if its log stays still for 45 min. A stuck
# lift showed 38 % all night (issue #4); the longest silent stretch it legitimately has is the final llvm-link (≤ 20 min).
# Before the kill, each python of the tree writes where it waits into the log (SIGUSR1, tools/boot.py), one at a time.
pile(){ local c; kill -USR1 "$1" 2>/dev/null && sleep 1; for c in $(pgrep -P "$1"); do pile "$c"; done; }
veille(){ local log=$1 p; shift; "$@" > "$log" 2>&1 & p=$!
  while kill -0 $p 2>/dev/null; do sleep ${VEILLE_PAS:-30}
    [ $(( $(date +%s) - $(stat -f %m "$log") )) -lt ${VEILLE_S:-2700} ] || { pile $p; tue $p; wait $p; return 124; }; done
  wait $p; }
garde(){ [ "$(libre)" -ge 8 ] || die "less than 8 GB free ($(libre)): free up some space, then run again (it resumes)"; }
for d in llvm@17 cmake ninja libvpx unicorn; do [ -d /opt/homebrew/opt/$d ] || die "missing: brew install $d"; done
python3 -c "import capstone" 2>/dev/null || die "missing: pip3 install capstone"
mkdir -p $BC; garde
say "0 %  Remill"
bash scripts/build_remill.sh || die "Remill build failed (see $BC/remill.log)"
say "10 %  Remill ready"
[ -s $BC/hlslcc_build/libhlslcc.dylib ] || {
  cmake -S third_party/HLSLcc -B $BC/hlslcc_build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null \
  && cmake --build $BC/hlslcc_build -j$J --target hlslcc > /dev/null; } || die "HLSLcc build failed (see preparation.log)"
# CMAKE_POLICY_VERSION_MINIMUM: HLSLcc asks for CMake 2.8, which CMake 4 refuses (issue #2)
[ -s runtime/win32/snd_reverb_gen.inc ] || BO2_REVERB_STEAM=1 bash scripts/gen_snd_reverb.sh

# The conversion stops after this mode (sps, zms or mps): "Play the campaign" writes sps, picking an unconverted mode in
# the game's menu raises it (scripts/play_steam.sh). No file: all three (issue #3).
FIN=$(cat $BC/conversion_target 2>/dev/null) || true; case "$FIN" in sps|zms) ;; *) FIN=mps ;; esac
P=10
for spec in "sps 9211 t6sp" "zms 12533 t6zm" "mps 12664 t6mp"; do
  set -- $spec; t=$1; N=$2; W=$BC/$t; mkdir -p $W
  if [ ! -f $W/.fini ]; then
    [ -s bin/ghidra-out-$t/calltree.csv ] || BO2_TARGET=$t python3 tools/ghidra_csv.py remplir "$S/$3.exe.unpacked.exe"
    [ -f $W/runway_vide.txt ] || : > $W/runway_vide.txt
    B="LIFT_PISTE_ENTIERE=1 BO2_TARGET=$t BOOT_WORKDIR=$W LIFT_ROOTS_FILE=tools/lift_roots_$t.txt LIFT_RUNWAY_FILE=$W/runway_vide.txt"
    if [ ! -f $W/.plie ]; then
      garde; say "$P %  $3: lift"
      veille $W/lift.log env $B python3 -u tools/boot.py -n $N -j $J --build-only || { [ $? = 124 ] \
        && die "$3 lift made no progress for 45 min (see $W/lift.log)"; die "$3 lift failed (see $W/lift.log)"; }
      garde; say "$((P + 15)) %  $3: fold"
      BOOT_WORKDIR=$W scripts/fold_lift.sh $F > $W/fold.log 2>&1 || die "$3 fold failed (see $W/fold.log)"
      mv ${W}_fold/lifted.o $W/lifted.o; rm -rf ${W}_fold; touch $W/.plie
      find $W -maxdepth 1 \( -name 'func_*.bc' -o -name 'func_*.bc.fixed' -o -name 'batch_*.bc' \) -delete   # the link only needs lifted.o and bc_files.txt
    fi
    say "$((P + 25)) %  $3: link"
    zsh tools/steam/native_impl.sh $t > $W/natif.log 2>&1 || die "$3 native fallbacks failed (see $W/natif.log)"
    env $B BOOT_RELINK_ONLY=1 python3 -u tools/boot.py -n $N -j $J --build-only > $W/link.log 2>&1 || die "$3 link failed (see $W/link.log)"
    [ -x $W/boot ] || die "$3 link failed (see $W/link.log)"
    touch $W/.fini
  fi
  P=$((P + 30))
  if [ $t = $FIN ]; then break; fi
done
say "100 %  done"

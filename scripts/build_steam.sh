#!/bin/bash
# build_steam.sh — translates the three Steam games from a fresh copy: Remill, then for campaign, Zombies and multiplayer
# the catalogue, lift, -O2 fold, native fallbacks and link. No byte of the game comes from the repository. Each step is
# skipped if its output exists, so a second run resumes. Progress: "[prepare] NN %" lines (tools/setup_window.swift).
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD; BC=boot_cache; S=~/.bo2-native/steam
C=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.ncpu); M=$(( $(sysctl -n hw.memsize) >> 32 ))
J=${J:-$(( C < M ? C : M ))}; [ "$J" -ge 1 ] || J=1; export J
F=$(( M * 3 / 4 )); [ "$F" -ge 1 ] || F=1   # fold slices are the memory-hungry step: 3 at 16 GB, more on larger Macs (only 16 GB measured)
say(){ echo "[prepare] $*"; }
die(){ say "❌ $*"; exit 1; }
libre(){ df -k "$ROOT" | awk 'NR==2{print int($4/1048576)}'; }
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

P=10
for spec in "sps 9211 t6sp" "zms 12533 t6zm" "mps 12664 t6mp"; do
  set -- $spec; t=$1; N=$2; W=$BC/$t; mkdir -p $W
  if [ ! -f $W/.fini ]; then
    [ -s bin/ghidra-out-$t/calltree.csv ] || BO2_TARGET=$t python3 tools/ghidra_csv.py remplir "$S/$3.exe.unpacked.exe"
    [ -f $W/runway_vide.txt ] || : > $W/runway_vide.txt
    B="LIFT_PISTE_ENTIERE=1 BO2_TARGET=$t BOOT_WORKDIR=$W LIFT_ROOTS_FILE=tools/lift_roots_$t.txt LIFT_RUNWAY_FILE=$W/runway_vide.txt"
    if [ ! -f $W/.plie ]; then
      garde; say "$P %  $3: lift"
      env $B python3 -u tools/boot.py -n $N -j $J --build-only > $W/lift.log 2>&1 || die "$3 lift failed (see $W/lift.log)"
      garde; say "$((P + 15)) %  $3: fold"
      BOOT_WORKDIR=$W scripts/fold_lift.sh $(( J < F ? J : F )) > $W/fold.log 2>&1 || die "$3 fold failed (see $W/fold.log)"
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
done
say "100 %  done"

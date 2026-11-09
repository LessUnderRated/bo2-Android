#!/bin/bash
# fold_lift.sh [parallel=2] [slice] — rebuild lifted.o from the lift's own slices, folded and at -O2.
#
# The lift (tools/boot.py --build-only) compiles lifted.o at -O0 from unfolded bitcode: every guest read/write,
# flag and compare is an out-of-line call and nothing lives in a register (sub_40b246: 13 x86 instructions ->
# 3 032 bytes of arm64). This replays the folding on $BOOT_WORKDIR/batch_N.bc — NO lift, NO remill — and writes
# ${BOOT_WORKDIR}_fold/lifted.o (scripts/build_steam.sh moves it into place). The -O0 object is kept as
# ${BOOT_WORKDIR}_fold/lifted_O0.o on the first run.
#
# Per slice: llvm-dis | stream_inline_wide | stream_fold (reads, flags, arm64 datalayout, noinline on sub_*)
#   | stream_inline_triv | stream_c27 | stream_inline_writes | dedup_ll_globals | widen_guest_gep | llvm-as
#   | clang -O2.  The `ret` fold (stream_inline_ret.py) on Android only: its short path now tests that the target
#   is inside the image (never heap code); anything else still reaches the runtime entry and its heap-code test.
# Android with BO2_FOLD_PASS=1: `opt -passes=bo2-dead-flags` (tools/llvm-pass, Rust plugin) between llvm-as and clang.
# With a second argument, only that slice is built (a dry run of the filters); nothing is joined.
set -u -o pipefail
cd "$(dirname "$0")/.."
export LC_ALL=C
L=${LLVM17_PREFIX:-/opt/homebrew/opt/llvm@17}/bin
# BO2_ANDROID=1: compile for aarch64-linux-android33 on Linux (GNU df/stat); macOS keeps its tools.
# -ffunction-sections on Android: ld.lld -r joins same-named sections, and one .text of several hundred MB leaves the
# final link no place for the range-extension thunks an arm64 branch (+-128 MB) needs. One section per function does.
if [ "${BO2_ANDROID:-}" = 1 ]; then CTGT="--target=aarch64-linux-android33 -ffunction-sections"; else CTGT="-arch arm64"; fi
taille(){ stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }
libre_g(){ df -Pk . | awk 'NR==2 {print int($4/1048576)}'; }
W=${BOOT_WORKDIR:?set BOOT_WORKDIR to the lift folder, as scripts/build_steam.sh does}
OUT=${W%_vm}_fold
PAR="${1:-2}"
ONLY="${2:-}"
say(){ echo "[$(date +%H:%M:%S)] [fold] $*"; }
die(){ say "ECHEC : $*"; exit 1; }
# stream_inline_wide goes FIRST: it refuses a stream that already defines __remill_read_memory_8/16, which is
# exactly what stream_fold appends (in the campaign `opt` had inlined those bodies away before this stage).
ECRITURES="python3 scripts/fold/stream_inline_writes.py | "
# Android : le `ret` aussi (stream_inline_ret.py) ; son repli fait maintenant le test du code du tas en ligne.
RET=""; [ "${BO2_ANDROID:-}" = 1 ] && RET="python3 scripts/fold/stream_inline_ret.py | "
# Android : chaque sequence `lock` prend la rayure de son adresse (stream_inline_atomic.py), avant tout autre
# filtre : il lui faut les appels __remill_read/write/compare_exchange_memory que les replis suivants font disparaitre.
ATOM=""; [ "${BO2_ANDROID:-}" = 1 ] && ATOM="python3 scripts/fold/stream_inline_atomic.py | "
# Android : en dernier, les metadonnees d'alias (State hors de portee de la memoire invitee et des chemins lents),
# les globaux du runtime en dso_local et les bases invariantes (stream_scope_state.py).
SCOPE=""; [ "${BO2_ANDROID:-}" = 1 ] && SCOPE=" | python3 scripts/fold/stream_scope_state.py"
FILTRES="${ATOM}python3 scripts/fold/stream_inline_wide.py | python3 scripts/fold/stream_fold.py \
  | python3 scripts/fold/stream_inline_triv.py | python3 scripts/fold/stream_c27.py \
  | ${ECRITURES}${RET}python3 scripts/fold/dedup_ll_globals.py | python3 scripts/fold/widen_guest_gep.py${SCOPE}"
# Android, BO2_FOLD_PASS=1 : la passe bo2-dead-flags (tools/llvm-pass/README.md) entre llvm-as et clang -O2. Elle
# efface les rangements de drapeaux x86 dans la State que la convention d'appel rend morts (morts a chaque retour et
# a chaque appel de code leve) : -7 % d'instructions arm64 sur les tranches mesurees. Le plugin est construit a part
# (cargo, README) et doit l'etre contre le meme LLVM 17 que $L/opt ; BO2_PASS_PLUGIN en donne le chemin.
PASS=""
if [ "${BO2_ANDROID:-}" = 1 ] && [ "${BO2_FOLD_PASS:-}" = 1 ]; then
  PASS=${BO2_PASS_PLUGIN:-${CARGO_TARGET_DIR:-/root/llvmpass/target}/release/libbo2_pass.so}
  [ -f "$PASS" ] || die "plugin absent : $PASS (cd tools/llvm-pass && cargo build --release)"
fi

pgrep -x boot >/dev/null && die "un boot tourne"
[ "$(libre_g)" -ge 8 ] || die "moins de 8 GiB libres"
mkdir -p $OUT
[ $OUT/lifted_O0.o -nt $W/lifted.o ] || cp -p $W/lifted.o $OUT/lifted_O0.o   # relift : la garde doit lire le nouvel objet

tranche() {   # $1 = slice number; writes $OUT/part_$1.o or $OUT/part_$1.ECHEC
  n="$1"; rm -f $OUT/part_$n.o $OUT/part_$n.ECHEC
  in=$W/cached_part_$n.bc; [ -f $in ] || in=$W/batch_$n.bc
  nice -n 20 bash -o pipefail -c "$L/llvm-dis -o - $in | $FILTRES | $L/llvm-as -o $OUT/part_$n.bc" \
      2> $OUT/part_$n.filtres || { echo filtres > $OUT/part_$n.ECHEC; return; }
  if [ -n "$PASS" ]; then   # opt verifie le module qu'il ecrit
    nice -n 20 $L/opt -load-pass-plugin=$PASS -passes=bo2-dead-flags -o $OUT/part_$n.p.bc $OUT/part_$n.bc \
        2>> $OUT/part_$n.filtres && mv -f $OUT/part_$n.p.bc $OUT/part_$n.bc || { echo passe > $OUT/part_$n.ECHEC; return; }
  fi
  nice -n 20 $L/clang $CTGT -c -O2 -x ir -o $OUT/part_$n.o $OUT/part_$n.bc 2> $OUT/part_$n.clang \
    || { echo clang > $OUT/part_$n.ECHEC; return; }
  rm -f $OUT/part_$n.bc
  echo "[$(date +%H:%M:%S)] [fold]   tranche $n : $(ls -la $OUT/part_$n.o | awk '{print $5}') octets"
}
export -f tranche taille; export L W OUT FILTRES CTGT PASS   # xargs runs `tranche` in a child bash

if [ -n "$ONLY" ]; then
  tranche "$ONLY"; cat $OUT/part_$ONLY.filtres; [ -f $OUT/part_$ONLY.ECHEC ] && die "tranche $ONLY : $(cat $OUT/part_$ONLY.ECHEC)"
  exit 0
fi
N=$(ls $W/cached_part_*.bc 2>/dev/null | wc -l); [ $N -gt 0 ] || N=$(ls $W/batch_*.bc | wc -l)
say "$N tranches, $PAR en parallele"
# Biggest slices first (same input as tranche()): in index order the largest ones came last and ran alone at the end
# (sps: 4 slices of 10-15 MB, the last minute; mps batch_124 is 2 min on its own).
for n in $(seq 0 $((N - 1))); do f=$W/cached_part_$n.bc; [ -f $f ] || f=$W/batch_$n.bc; echo "$(taille $f) $n"; done \
  | sort -rn | cut -d' ' -f2 | xargs -P "$PAR" -I{} bash -c 'tranche {}'
ls $OUT/*.ECHEC >/dev/null 2>&1 && die "tranches en echec : $(grep -H . $OUT/*.ECHEC | tr '\n' ' ')"
grep -h "fold\|\[ecrit\]\|\[large\]\|\[widen\]\|\[bo2-dead-flags\]" $OUT/part_0.filtres
# U: the C symbol prefix (Mach-O's "_", none on ELF); LDR: the relocatable link.
if [ "${BO2_ANDROID:-}" = 1 ]; then U=; LDR="$L/ld.lld -r"; NMT=$L/llvm-nm
else U=_; LDR="$L/clang -arch arm64 -r -nostdlib"; NMT=nm; fi
$L/clang $CTGT -c -O0 -x ir -o $OUT/stubs.o $W/stubs.bc || die "stubs"
$LDR -o $OUT/lifted.o $(seq -f "$OUT/part_%g.o" 0 $((N - 1))) $OUT/stubs.o || die "ld -r"

# Gates: the same lifted entries as the -O0 object, and no naked accessor left.
$NMT $OUT/lifted_O0.o | awk -v p="^${U}sub_[0-9a-f]+$" '$2=="T" && $3 ~ p {print $3}' | sort > $OUT/subs_O0.txt
$NMT $OUT/lifted.o    | awk -v p="^${U}sub_[0-9a-f]+$" '$2=="T" && $3 ~ p {print $3}' | sort > $OUT/subs_O2.txt
cmp -s $OUT/subs_O0.txt $OUT/subs_O2.txt || die "les entrees sub_ different de l'objet -O0 ($(wc -l < $OUT/subs_O0.txt) / $(wc -l < $OUT/subs_O2.txt))"
NUS=$($NMT -u $OUT/lifted.o | grep -E "${U}__remill_(read|write)_memory_(8|16|32|f32|64|f64)\$|${U}__remill_flag_computation|${U}__remill_compare_|${U}__remill_undefined_" | grep -v compare_exchange | tr '\n' ' ')
[ -z "$NUS" ] || die "accesseurs nus restants : $NUS"
# The relink redirects entries by writing 3 instructions (tools/patch_redirect.py: adrp, add, br) over the first
# 12 bytes of `_sub_<va>`: a shorter function would have its neighbour overwritten. Android patches no entry (its
# redirects are weak lifted symbols and strong trampolines at the link, Bo2-APK's scripts/package-game.sh).
COURT=
[ -z "$U" ] || COURT=$(nm -n $OUT/lifted.o | python3 -c '
import sys   # macOS awk has no strtonum
r = [(int(a, 16), n) for a, t, n in (l.split() for l in sys.stdin if len(l.split()) == 3) if t in "tT"]
print(" ".join(f"{n}:{b - a}" for (a, n), (b, _) in zip(r, r[1:]) if n.startswith("_sub_") and b - a < 12))')
[ -z "$COURT" ] || die "fonctions de moins de 12 octets (redirection d'entree impossible) : $COURT"
say "OK : $(wc -l < $OUT/subs_O2.txt | tr -d ' ') entrees, $(ls -la $OUT/lifted.o | awk '{print $5}') octets (-O0 : $(ls -la $OUT/lifted_O0.o | awk '{print $5}'))"

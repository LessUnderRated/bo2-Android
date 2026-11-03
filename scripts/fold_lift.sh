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
#   | clang -O2.  Left out on purpose: the filters that change what a `ret` sees (every `ret` reaches the runtime
#   entry, whose short path keeps the heap-code test).
# With a second argument, only that slice is built (a dry run of the filters); nothing is joined.
set -u -o pipefail
cd "$(dirname "$0")/.."
export LC_ALL=C
L=/opt/homebrew/opt/llvm@17/bin
W=${BOOT_WORKDIR:?set BOOT_WORKDIR to the lift folder, as scripts/build_steam.sh does}
OUT=${W%_vm}_fold
PAR="${1:-2}"
ONLY="${2:-}"
say(){ echo "[$(date +%H:%M:%S)] [fold] $*"; }
die(){ say "ECHEC : $*"; exit 1; }
# stream_inline_wide goes FIRST: it refuses a stream that already defines __remill_read_memory_8/16, which is
# exactly what stream_fold appends (in the campaign `opt` had inlined those bodies away before this stage).
ECRITURES="python3 scripts/fold/stream_inline_writes.py | "
FILTRES="python3 scripts/fold/stream_inline_wide.py | python3 scripts/fold/stream_fold.py \
  | python3 scripts/fold/stream_inline_triv.py | python3 scripts/fold/stream_c27.py \
  | ${ECRITURES}python3 scripts/fold/dedup_ll_globals.py | python3 scripts/fold/widen_guest_gep.py"

pgrep -x boot >/dev/null && die "un boot tourne"
[ "$(df -g . | awk 'NR==2 {print $4}')" -ge 8 ] || die "moins de 8 GiB libres"
mkdir -p $OUT
[ $OUT/lifted_O0.o -nt $W/lifted.o ] || cp -p $W/lifted.o $OUT/lifted_O0.o   # relift : la garde doit lire le nouvel objet

tranche() {   # $1 = slice number; writes $OUT/part_$1.o or $OUT/part_$1.ECHEC
  n="$1"; rm -f $OUT/part_$n.o $OUT/part_$n.ECHEC
  in=$W/cached_part_$n.bc; [ -f $in ] || in=$W/batch_$n.bc
  nice -n 20 bash -o pipefail -c "$L/llvm-dis -o - $in | $FILTRES | $L/llvm-as -o $OUT/part_$n.bc" \
      2> $OUT/part_$n.filtres || { echo filtres > $OUT/part_$n.ECHEC; return; }
  nice -n 20 $L/clang -arch arm64 -c -O2 -x ir -o $OUT/part_$n.o $OUT/part_$n.bc 2> $OUT/part_$n.clang \
    || { echo clang > $OUT/part_$n.ECHEC; return; }
  rm -f $OUT/part_$n.bc
  echo "[$(date +%H:%M:%S)] [fold]   tranche $n : $(ls -la $OUT/part_$n.o | awk '{print $5}') octets"
}
export -f tranche; export L W OUT FILTRES   # xargs runs `tranche` in a child bash

if [ -n "$ONLY" ]; then
  tranche "$ONLY"; cat $OUT/part_$ONLY.filtres; [ -f $OUT/part_$ONLY.ECHEC ] && die "tranche $ONLY : $(cat $OUT/part_$ONLY.ECHEC)"
  exit 0
fi
N=$(ls $W/cached_part_*.bc 2>/dev/null | wc -l); [ $N -gt 0 ] || N=$(ls $W/batch_*.bc | wc -l)
say "$N tranches, $PAR en parallele"
seq 0 $((N - 1)) | xargs -P "$PAR" -I{} bash -c 'tranche {}'
ls $OUT/*.ECHEC >/dev/null 2>&1 && die "tranches en echec : $(grep -H . $OUT/*.ECHEC | tr '\n' ' ')"
grep -h "fold\|\[ecrit\]\|\[large\]\|\[widen\]" $OUT/part_0.filtres
$L/clang -arch arm64 -c -O0 -x ir -o $OUT/stubs.o $W/stubs.bc || die "stubs"
$L/clang -arch arm64 -r -nostdlib -o $OUT/lifted.o $(seq -f "$OUT/part_%g.o" 0 $((N - 1))) $OUT/stubs.o || die "ld -r"

# Gates: the same lifted entries as the -O0 object, and no naked accessor left.
nm $OUT/lifted_O0.o | awk '$2=="T" && $3 ~ /^_sub_[0-9a-f]+$/ {print $3}' | sort > $OUT/subs_O0.txt
nm $OUT/lifted.o    | awk '$2=="T" && $3 ~ /^_sub_[0-9a-f]+$/ {print $3}' | sort > $OUT/subs_O2.txt
cmp -s $OUT/subs_O0.txt $OUT/subs_O2.txt || die "les entrees _sub_ different de l'objet -O0 ($(wc -l < $OUT/subs_O0.txt) / $(wc -l < $OUT/subs_O2.txt))"
NUS=$(nm -u $OUT/lifted.o | grep -E '___remill_(read|write)_memory_(8|16|32|f32|64|f64)$|___remill_flag_computation|___remill_compare_|___remill_undefined_' | grep -v compare_exchange | tr '\n' ' ')
[ -z "$NUS" ] || die "accesseurs nus restants : $NUS"
# The relink redirects entries by writing 3 instructions (tools/patch_redirect.py: adrp, add, br) over the first
# 12 bytes of `_sub_<va>`: a shorter function would have its neighbour overwritten.
COURT=$(nm -n $OUT/lifted.o | python3 -c '
import sys   # macOS awk has no strtonum
r = [(int(a, 16), n) for a, t, n in (l.split() for l in sys.stdin if len(l.split()) == 3) if t in "tT"]
print(" ".join(f"{n}:{b - a}" for (a, n), (b, _) in zip(r, r[1:]) if n.startswith("_sub_") and b - a < 12))')
[ -z "$COURT" ] || die "fonctions de moins de 12 octets (redirection d'entree impossible) : $COURT"
say "OK : $(wc -l < $OUT/subs_O2.txt | tr -d ' ') entrees, $(ls -la $OUT/lifted.o | awk '{print $5}') octets (-O0 : $(ls -la $OUT/lifted_O0.o | awk '{print $5}'))"

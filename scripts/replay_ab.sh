#!/bin/bash
# replay_ab.sh — STAGE 3: the same run twice, ONE FLAG APART.
#
#   scripts/replay_ab.sh <outdir> <journal> <runner> <FLAG=VALUE> [secs]
#
# Both sides replay the SAME journal, so every host input — clock, entropy,
# cursor, input messages, audio credits — is byte-identical between them. The
# only difference in the universe is the flag. [IMP-HASH] then names the exact
# 100,000-import window in which the flag FIRST changes the program.
#
# WHY THIS IS THE POINT OF ALL THE REPLAY WORK. 11 of this project's 93
# retractions (docs/RETRACTIONS.md, family C) died because two runs of the same
# build with the same flags disagree, so no A/B could ever be trusted; 10 more
# (family D) because a change of mine produced the symptom I was investigating.
# Both classes collapse to this one comparison.
#
# ⚠️ A flag that changes NOTHING prints identical hashes throughout — and that is
# a RESULT, not a failure. dcbe708 retracted "BO2_LJ_GUARD unlocks the server" on
# a hand-run A/B; this is the same test with the noise removed.
set -u
OUT="${1:?usage: replay_ab.sh <outdir> <journal> <runner> <FLAG=VALUE> [secs]}"
JRNL="${2:?need a journal}"
RUNNER="${3:?need a runner script}"
FLAG="${4:?need FLAG=VALUE}"
SECS="${5:-150}"
cd "$(dirname "$0")/.."
mkdir -p "$OUT"
[ -f "$JRNL" ] || { echo "no such journal: $JRNL" >&2; exit 2; }
case "$FLAG" in *=*) ;; *) echo "FLAG must be NAME=VALUE" >&2; exit 2 ;; esac

echo "=== A: WITHOUT $FLAG ==="
"$RUNNER" "$OUT/a" "$SECS" BO2_REPLAY="$PWD/$JRNL" 2>&1 | tail -4
echo "=== B: WITH $FLAG ==="
"$RUNNER" "$OUT/b" "$SECS" BO2_REPLAY="$PWD/$JRNL" "$FLAG" 2>&1 | tail -4

echo; echo "=== WHERE THE FLAG FIRST BITES ==="
python3 - "$OUT/a/run.log" "$OUT/b/run.log" "$FLAG" <<'PY'
import sys
def h(p):
    try: return [l.strip() for l in open(p, errors='replace') if l.startswith('[IMP-HASH]')]
    except OSError: return []
A, B, flag = h(sys.argv[1]), h(sys.argv[2]), sys.argv[3]
print(f"  A (without): {len(A)} hash lines   B (with {flag}): {len(B)}")
# ⚠️ REFUSE TO CONCLUDE FROM A DEGENERATE COMPARISON. The first run of this
# script printed "BO2_ITER=1 CHANGES NOTHING for the first 0 imports" because
# side A had died after 10,316 lines and produced ONE hash line, so "all 1 common
# hash lines identical" was true and meaningless. That is the same shape as every
# capped-probe retraction in docs/RETRACTIONS.md, committed by the tool built to
# end them. A comparison needs BOTH sides to have actually run.
MIN_LINES = 3
if len(A) < MIN_LINES or len(B) < MIN_LINES:
    short = "A" if len(A) < MIN_LINES else "B"
    print(f"\n  ⛔ NO VERDICT. Side {short} produced too few hash lines "
          f"(A={len(A)}, B={len(B)}, need >= {MIN_LINES} each).")
    print(f"  A run that died early cannot be compared to one that did not, and")
    print(f"  'the common prefix matched' is trivially true when the prefix is 1.")
    print(f"  Check {sys.argv[1]} and {sys.argv[2]} for why the short side stopped.")
    sys.exit(2)
n = min(len(A), len(B))
for i in range(n):
    if A[i] != B[i]:
        lo, hi = max(0, i-1)*100000, i*100000
        print(f"\n  🎯 {flag} FIRST CHANGES THE RUN in imports {lo}..{hi}")
        print(f"     A: {A[i]}")
        print(f"     B: {B[i]}")
        print(f"     identical for the {i} hash lines before it")
        sys.exit(0)
print(f"\n  {flag} CHANGES NOTHING for the first {(n-1)*100000} imports —")
print(f"  all {n} common hash lines identical. With every host input held")
print(f"  byte-identical by the journal, that is a real negative, not noise.")
PY

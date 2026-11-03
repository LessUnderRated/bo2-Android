#!/bin/bash
# until_teardown.sh — retry angola until a run reaches the TEARDOWN FREEZE, and
# leave a replayable journal behind when it does.
#
#   ATTEMPTS=6 SECS=300 scripts/until_teardown.sh runs/td [BO2_X=1 ...]
#
# ⚠️ WHY NOT scripts/until_freeze.sh. That one drives scripts/play.sh, and
#    play.sh has been measured never to load the level zone (angola.ff appears in
#    0 of 40 ZONE-LOAD lines). Every measurement from such a run is about a run
#    that did not happen. angola_run.sh is the reference runner.
#
# ⚠️ THE SUCCESS TEST IS THE FRAME COUNTER, NOT A TAG. `[FROZEN #1]` fires after
#    ONE HUD sample with no new Present, so it also fires during boot, before the
#    first frame exists — the boot block cost two sessions of mode-B analysis.
#    A real freeze is the LAST [CADENCE] line reading guestFrames=N(+0) with N
#    large. A run that ends while frames are still advancing is a MISS, and this
#    says so rather than leaving a healthy run to be mined for a freeze.
#
# The freeze is not deterministic: runs/path3 froze at frame 3660 while runs/fv1
# reached 3890 and was still advancing at the budget. So retry, and record — a
# journal of a run that DID freeze can be replayed under new probes for free.
set -u
OUT="${1:?usage: until_teardown.sh <outdir-prefix> [BO2_X=1 ...]}"; shift
SECS="${SECS:-300}"
cd "$(dirname "$0")/.."

for a in $(seq 1 "${ATTEMPTS:-6}"); do
    d="${OUT}$a"
    avail=$(df -g . | awk 'NR==2{print $4}')
    if [ "$avail" -lt 8 ]; then
        echo "[until_teardown] only ${avail}G free — gzipping old logs"
        find runs -name run.log -mmin +60 -exec gzip -f {} \; 2>/dev/null
    fi
    echo "[until_teardown] attempt $a/${ATTEMPTS:-6} -> $d  (${avail}G free)"
    bash scripts/angola_run.sh "$d" "$SECS" BO2_REC="$d/jrnl.bin" "$@" >/dev/null 2>&1

    last=$(LC_ALL=C grep 'guestFrames=' "$d/run.log" 2>/dev/null | tail -1)
    gf=$(printf '%s' "$last" | sed -n 's/.*guestFrames=\([0-9]*\)(\([+-][0-9]*\)).*/\1 \2/p')
    n=$(printf '%s' "$gf" | cut -d' ' -f1); dlt=$(printf '%s' "$gf" | cut -d' ' -f2)
    n=${n:-0}; dlt=${dlt:-+1}
    if [ "$n" -gt 500 ] && [ "$dlt" = "+0" ]; then
        echo "[until_teardown] 🎯 FROZE at frame $n — $d (journal $d/jrnl.bin)"
        LC_ALL=C grep -E '^\[FREEVARS\]|^\[VARPOOL\]' "$d/run.log" | head -20
        exit 0
    fi
    echo "[until_teardown]   miss: last guestFrames=$n($dlt) — still advancing, not a freeze"
    gzip -f "$d/run.log" 2>/dev/null
done
echo "[until_teardown] no freeze in ${ATTEMPTS:-6} attempts"
exit 1

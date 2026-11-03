#!/bin/bash
# ── ONE RELIABLE RUN, ONE FIXED METRIC LINE ──────────────────────────────────
#
#   scripts/run_level.sh <level> [seconds-after-load] [VAR=VAL ...]
#
# Written because hand-rolled polling loops cost three wrong conclusions in a
# single day: `timeout` does not exist on macOS, a loop that exits before the
# event cannot observe it, and a capped probe's LINE count is not its EVENT
# count. Every experiment should print the SAME line so two runs can be
# subtracted rather than eyeballed.
#
# It also retries once when the run dies before the level loads, because two
# intermittent SIGSEGVs kill roughly half of all runs and a dead run is not a
# result.
set -u
LEVEL="${1:-afghanistan}"; shift || true
HOLD="${1:-60}"; case "$HOLD" in ''|*[!0-9]*) HOLD=60 ;; *) shift || true ;; esac
LOG="runs/play-$LEVEL/run.log"

one_attempt() {
    pkill -9 -f boot_cache/boot 2>/dev/null
    for _ in $(seq 1 12); do sleep 1
        [ "$(ps -Ao args | grep -c '[b]oot_cache/boot')" -eq 0 ] && break; done
    rm -f "$LOG"
    nohup scripts/play.sh "$LEVEL" "$@" >/tmp/run_level.out 2>&1 &
    local waited=0
    while [ $waited -lt 300 ]; do
        sleep 10; waited=$((waited+10))
        [ "$(ps -Ao args | grep -c '[b]oot_cache/boot')" -eq 0 ] && return 2   # died early
        [ "$(grep -ac "$LEVEL\.ff" "$LOG" 2>/dev/null)" -ge 3 ] && return 0    # level in
    done
    return 3                                                                   # never loaded
}

one_attempt "$@"; rc=$?
if [ $rc -eq 2 ]; then
    echo "[harness] the run died before the level loaded — retrying once"
    one_attempt "$@"; rc=$?
fi
[ $rc -ne 0 ] && { echo "[harness] FAILED rc=$rc (2=died early, 3=never loaded)"; exit $rc; }

sleep "$HOLD"
alive=$(ps -Ao args | grep -c '[b]oot_cache/boot')
f1=$(grep -ao 'guestFrames=[0-9]*' "$LOG" 2>/dev/null | tail -1 | tr -dc 0-9)
sleep 20
f2=$(grep -ao 'guestFrames=[0-9]*' "$LOG" 2>/dev/null | tail -1 | tr -dc 0-9)
bash scripts/shot.sh "runs/$LEVEL-latest.png" >/dev/null 2>&1

# THE fixed line. Same fields, same order, every time.
echo "[METRICS] level=$LEVEL alive=$alive frames=${f1:-0} frames+20s=${f2:-0}" \
     "advancing=$([ "${f2:-0}" -gt "${f1:-0}" ] && echo YES || echo NO)" \
     "gscRun=$(grep -ao 'gscRun=[0-9]*' "$LOG" 2>/dev/null | tail -1 | tr -dc 0-9)" \
     "svWorker=$(grep -ao 'svWorker=[0-9]*' "$LOG" 2>/dev/null | tail -1 | tr -dc 0-9)" \
     "draws=$(grep -ao 'this frame [0-9]* records' "$LOG" 2>/dev/null | tail -1 | tr -dc 0-9)" \
     "crash=$(grep -ac 'signal=' "$LOG" 2>/dev/null)" \
     "runaway=$(grep -ac RUNAWAY "$LOG" 2>/dev/null)"

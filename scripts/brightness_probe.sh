#!/bin/bash
# Try the engine's own rendering levers on a RUNNING game, one at a time, and
# measure what each one does to the picture.
#
# WHY. afghanistan renders BLOWN OUT (mean 190, a flat beige) and angola renders
# NEARLY BLACK — opposite directions through the same composite, which is why one
# exposure control being wrong is a better hypothesis than two separate bugs.
# Guessing which lever matters from the shader is how this project lost two hours
# to the colour-grading LUT, which turned out to be correct. So: change ONE thing,
# photograph, measure, restore. The engine already has the levers and BO2_CMDPIPE
# can now deliver them to a game that is already running.
#
# Every name here was verified present in the image as a null-terminated string
# before being sent (GUIDE rule 4); an unregistered command dies in SILENCE, and
# the runtime's [CMDPIPE] lines say which of those happened.
#
#   scripts/brightness_probe.sh <outdir> [fifo]
set -u
OUT="${1:?usage: brightness_probe.sh <outdir> [cmdpipe]}"
FIFO="${2:-/tmp/bo2.cmd}"
cd "$(dirname "$0")/.."
mkdir -p "$OUT"

pgrep -f '\./boot_cache/boot ' >/dev/null || {
    echo "no game is running — start one with scripts/play.sh first" >&2; exit 1; }
[ -p "$FIFO" ] || { echo "$FIFO is not a FIFO — was the run started with BO2_CMDPIPE?" >&2; exit 1; }

shoot() {  # shoot <label>
    ./scripts/shot.sh "$OUT/$1.png" >/dev/null 2>&1 || true
    if [ -s "$OUT/$1.png" ]; then
        printf "  %-22s " "$1"
        python3 tools/pxstat.py "$OUT/$1.png" 2>/dev/null | sed 's/^[^ ]* *//'
    else
        echo "  $1: ⚠️  NO PICTURE"
    fi
}

echo "baseline first — every later number is only meaningful against it"
shoot 00_baseline

# One lever at a time, each restored before the next, so the measurements do not
# compound. The value after the name is the RESTORE value.
try() {  # try <label> <set-command> <restore-command>
    ./scripts/sendcmd "$FIFO" $2 >/dev/null 2>&1 || true
    sleep 3
    shoot "$1"
    ./scripts/sendcmd "$FIFO" $3 >/dev/null 2>&1 || true
    sleep 2
}

# ⚠️ SEND THEM AS DVAR ASSIGNMENTS, NOT AS COMMANDS.
#
# The first version sent bare "r_filmEnable 0". The runtime answered:
#
#   [CMDPIPE] injecting "r_filmEnable 0" — ⚠️ "r_filmEnable" is NOT REGISTERED
#             on the console list; it will be dispatched and dropped in SILENCE
#
# which is right: these are DVARS, and the registered command that assigns a
# dvar is `set`. Every lever below therefore goes through `set`, and the
# [CMDPIPE] line for `set` should no longer carry the not-registered warning —
# check that before reading any measurement, because a silently dropped command
# and a lever that genuinely does nothing look identical in the picture.
try 01_fullbright   "set r_fullbright 1"   "set r_fullbright 0"
try 02_filmoff      "set r_filmEnable 0"   "set r_filmEnable 1"
try 03_fogoff       "set r_fog 0"          "set r_fog 1"
try 04_gamma_up     "set r_gamma 2.5"      "set r_gamma 1"
try 05_brightness   "set r_brightness 0.8" "set r_brightness 0"
try 06_contrast     "set r_contrast 1.5"   "set r_contrast 1"

echo
echo "final state (everything restored):"
shoot 07_restored
echo
echo "A lever that moved the mean is real. A lever that changed NOTHING is either"
echo "not registered or points at the empty ret — check the run log's [CMDPIPE]"
echo "lines, which say which, rather than assuming the lever is wrong."

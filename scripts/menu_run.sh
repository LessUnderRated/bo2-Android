#!/bin/bash
# Boot to the 3D main menu, photograph it, and dump every render target.
#
# WHY THIS EXISTS: doing it by hand meant resolving the pid with
# `pgrep -f boot_cache/boot | head -1`, and when a previous run had not exited
# that returns the OLD process — so the Return keystroke and the USR1 went to a
# process that was still on the title screen, and the run produced no dumps at
# all while looking like it had worked. Capture the pid at launch, refuse to
# start with another instance alive, and wait for the menu instead of sleeping a
# guessed number of seconds.
#
# ⚠️ THIS IS A MENU WITNESS, NOT A LEVEL RUN. It sets BO2_NOVIDEO=1, and 8d7e14e
# measured that the intro path is LOAD-BEARING for the subsequent zone load — with
# videos off, a campaign level never loads at all. Never judge level work from here.
#
# ⚠️ KEYS GO THROUGH BO2_KEYPIPE. Until 2026-09-01 this script called
# scripts/sendkey directly, which is CGEventPostToPid — GUIDE §9: an INACTIVE
# macOS app never receives the event, four runs died that way and none of them
# printed anything. This script has not run since 2026-07-27, i.e. it has never
# run since that was discovered. It now opens a FIFO and reports SENT vs
# REGISTERED separately, because those two numbers failing apart is the symptom.
#
#   scripts/menu_run.sh <outdir> [extra BO2_* env assignments...]
set -u
OUT="${1:?usage: menu_run.sh <outdir> [VAR=VAL ...]}"; shift
mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/run.log

AVAIL_G=$(df -g . | awk 'NR==2{print $4}')
SWAP_FREE=$(sysctl -n vm.swapusage | sed -E 's/.*free = ([0-9]+).*/\1/')
echo "disk ${AVAIL_G} GiB free, swap ${SWAP_FREE} MB free"
if [ "${AVAIL_G:-99}" -lt 4 ]; then
    echo "REFUSING: under 4 GiB free — macOS cannot create swap and the box hard-hangs" >&2
    exit 3
fi

# ⚠️ MATCH THE PROCESS NAME, NOT THE COMMAND LINE. `pgrep -f <anything>` also
# matches a shell that merely MENTIONS the path, so a monitoring loop waiting for
# the game to exit refuses the next run — that happened twice today, once with a
# loose pattern and once with an anchored one. `pgrep -x boot` matches the
# executable's name, which a shell cannot impersonate.
if pgrep -x boot > /dev/null; then
    echo "REFUSING: another boot_cache/boot is already running:" >&2
    pgrep -xl boot >&2
    exit 1
fi

cd "$(dirname "$0")/.."
KEYPIPE="${BO2_KEYPIPE:-$TMPDIR/bo2.menu.key}"
rm -f "$KEYPIPE"; mkfifo "$KEYPIPE" || { echo "cannot create $KEYPIPE" >&2; exit 3; }
export BO2_KEYPIPE="$KEYPIPE"
env DYLD_LIBRARY_PATH="$PWD/boot_cache/hlslcc_build" \
    BO2_STALL_ABORT=0 BO2_SV_INLINE=1 BO2_WINDOW=1 BO2_XAUDIO=1 \
    BO2_SND_UNMUTE=1 BO2_NOVIDEO=1 \
    BO2_KEYPIPE="$KEYPIPE" \
    BO2_SHOT="$OUT/screen" BO2_RT_DUMP="$OUT/rt" \
    "$@" \
    ./boot_cache/boot boot_cache/pe_sections.bin > "$OUT/run.log" 2>&1 &
PID=$!
echo "pid=$PID  log=$OUT/run.log"

# Wait for the TITLE screen by its own evidence — the shader translation
# settling down — rather than by a fixed sleep that was tuned on one machine.
for i in $(seq 1 90); do
    sleep 2
    kill -0 "$PID" 2>/dev/null || { echo "DIED before the title screen"; tail -5 "$OUT/run.log"; exit 2; }
    n=$(python3 -c "print(open('$OUT/run.log',errors='replace').read().count('screen painters'))")
    [ "$n" -ge 3 ] && { echo "title screen up after $((i*2))s"; break; }
done

./scripts/sendkey.sh "$PID" 0x24 && echo "Return sent"
# WAIT FOR THE MENU, DO NOT SLEEP A GUESS. A fixed 25 s photographed the TITLE
# screen once already and the whole run was wasted comparing the wrong picture.
# The 3D menu's own signature is the number of distinct pixel shaders painting
# the screen: 2 on the title screen, 6+ once the frontend room is up.
for i in $(seq 1 60); do
    sleep 2
    kill -0 "$PID" 2>/dev/null || { echo "DIED before the menu"; tail -5 "$OUT/run.log"; exit 2; }
    d=$(python3 -c "
import re
n=0
for l in open('$OUT/run.log',errors='replace'):
    if 'screen painters' in l:
        m=re.search(r'(\d+) distinct', l)
        if m: n=int(m.group(1))
print(n)")
    [ "$d" -ge 6 ] && { echo "3D menu up after $((i*2))s (${d} distinct screen painters)"; break; }
done
# BO2_RT_SWEEP dumps on its own every frame, so it needs TIME, not a signal.
if printf '%s\n' "$@" | grep -q RT_SWEEP; then
    echo "sweep mode: collecting frames for 120 s"
    sleep 120
else
    kill -USR1 "$PID" && echo "USR1 sent (shot + RT dump)"
    sleep 12
fi
kill "$PID" 2>/dev/null
sleep 2
echo "--- results ---"
python3 -c "
import sys
for l in open('$OUT/run.log',errors='replace'):
    if l.startswith(('[RT-DUMP]','[PS-SKIP]','[LUT-USE]','[SHOT] frame')) or 'screen painters' in l:
        print(l.rstrip()[:220])
" | tail -40
ls "$OUT"/*.png 2>/dev/null

# THE WITNESS'S OWN VERDICT. Two counts, never one: GUIDE §9 — a key can be
# delivered and still be ignored, and only for SOME keys. [KEYPIPE] is what the
# runtime accepted, [KB-NAV] is what the engine's translator registered (twice
# per press, key down and key up). Reporting only the first is how four runs
# read as successes.
echo "--- verdict ---"
python3 - "$OUT/run.log" <<'PY'
import sys, re
log = open(sys.argv[1], errors='replace').read()
sent      = log.count('[KEYPIPE] key')
registered= log.count('[KB-NAV] key')
painters  = max([int(m) for l in log.splitlines() if 'screen painters' in l
                     for m in re.findall(r'(\d+) distinct', l)] or [0])
shots     = log.count('[SHOT] frame')
print("keys SENT (KEYPIPE)      %d" % sent)
print("keys REGISTERED (KB-NAV) %d" % registered)
print("max distinct screen painters %d   (2 = title screen, 6+ = 3D menu room)" % painters)
print("screenshots written          %d" % shots)
ok = painters >= 6 and shots >= 1
print("VERDICT: %s" % ("HEALTHY — the 3D menu came up and was photographed" if ok
      else "NOT the known-good state; see the counts above"))
sys.exit(0 if ok else 1)
PY
V=$?
rm -f "$KEYPIPE"
exit $V

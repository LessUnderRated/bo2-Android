#!/bin/bash
# Photograph the LEVEL, not the clock.
#
# WHY THIS EXISTS. campaign_run.sh shoots every 60 s from launch. The campaign
# does not reach the level until ~11-14 minutes in (line ~167k of ~173k), and on
# several runs the shot loop stopped after 3 minutes — so every screenshot this
# project has of a "loaded level" is actually the loading screen, and the only
# thing anyone has ever concluded from them is "it is black", which was never
# about the level at all.
#
# This waits for the level to actually be up and then photographs it repeatedly,
# because the interesting question is whether the picture CHANGES once the guest
# is presenting frames again — one still cannot tell a fade-in from a dead
# renderer.
#
# ⚠️ THE TRIGGER USED TO BE [SETORIGIN] ALONE, AND THAT CANNOT FIRE ON THE ONE
# LEVEL KNOWN TO RENDER. afghanistan's zone contains no level script (60d60cc:
# "maps/afghanistan.gsc *** MISSING ***"), so nothing ever places the player and
# [SETORIGIN] counts ZERO — verified in runs/afg1's own log, the run that
# produced the screenshots the tester saw. Pointed at afghanistan this script would
# have waited its full deadline and taken NOT ONE PICTURE, which is precisely the
# failure it was written to end.
#
# So there are two triggers, and the script SAYS WHICH ONE FIRED:
#   [SETORIGIN]        the player was placed — angola and any scripted level
#   draw RATE          [D3D-DRAW] prints once per 1000 draws and is never gated,
#                      so its rate separates the menu (~1 draw/frame) from a
#                      level (~1056 draws/frame) on ANY map, script or no script
# and if neither fires it still photographs before exiting, saying so. A run that
# ends with zero pictures teaches nothing.
#
#   scripts/shoot_the_level.sh <outdir> [minutes] [BO2_* env assignments...]
set -u
OUT="${1:?usage: shoot_the_level.sh <outdir> [minutes] [VAR=VAL ...]}"; shift
MINUTES="${1:-16}"; [ $# -gt 0 ] && shift
cd "$(dirname "$0")/.."
LOG="$OUT/run.log"

# Hoist the variables THIS SCRIPT ITSELF reads out of the VAR=VAL argument list.
# They are already forwarded to the guest through "$@", but a `VAR=VAL` argument
# sets nothing in our own environment, so `${BO2_RT_DUMP:-}` below would read
# empty and the dump would silently never happen — the same shape of bug
# campaign_run.sh's header records costing a whole run, one level up.
for a in "$@"; do case "$a" in
    BO2_LEVEL=*)     BO2_LEVEL="${a#BO2_LEVEL=}";         export BO2_LEVEL ;;
    BO2_CLICKPIPE=*) BO2_CLICKPIPE="${a#BO2_CLICKPIPE=}"; export BO2_CLICKPIPE ;;
    BO2_RT_DUMP=*)   BO2_RT_DUMP="${a#BO2_RT_DUMP=}";     export BO2_RT_DUMP ;;
esac; done

if pgrep -f '\./boot_cache/boot ' > /dev/null; then
    echo "REFUSING: another boot_cache/boot is already running:" >&2
    pgrep -fl '\./boot_cache/boot ' >&2; exit 1
fi
mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/run.log

env DYLD_LIBRARY_PATH="$PWD/boot_cache/hlslcc_build" \
    BO2_STALL_ABORT=0 BO2_SV_INLINE=1 BO2_WINDOW=1 BO2_XAUDIO=1 \
    BO2_SND_UNMUTE=1 BO2_UIX_PROBE=1 BO2_VPX_NATIVE=1 BO2_VPX_REPLACE=1 \
    BO2_MUTE="${BO2_MUTE:-0}" \
    BO2_FORCE_MAP_NOW="${BO2_LEVEL:-afghanistan}" \
    "$@" \
    ./boot_cache/boot boot_cache/pe_sections.bin > "$LOG" 2>&1 &
PID=$!
echo "pid=$PID log=$LOG level=${BO2_LEVEL:-afghanistan} — waiting for the LEVEL, not the clock"

DEADLINE=$(( $(date +%s) + MINUTES * 60 ))

# One [D3D-DRAW] line per 1000 draws, and the site is NOT env-gated, so this
# counter exists in every run. Its RATE is the trigger; see the header.
#
# ⚠️ MUST return a bare integer on every path, or it silently disables the whole
# trigger. `grep -c` prints "0" AND exits 1 when there are no matches, so the
# obvious `grep -c … || echo 0` emits "0\n0"; `$(( CUR - PREV ))` then dies with
# "syntax error (error token is \"0\")" and the run photographs nothing but the
# loading screen. Measured, on the first launch of this very run.
draw_lines() {
    local n
    n=$(grep -c '\[D3D-DRAW\]' "$LOG" 2>/dev/null | head -1)
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    printf '%s' "$n"
}

photograph() {
    echo "  *** $1 — photographing. ***"
    for i in $(seq 1 12); do
        kill -0 "$PID" 2>/dev/null || break
        f="$OUT/level_$(printf '%02d' "$i").png"
        ./scripts/shot.sh "$f" >/dev/null 2>&1 || true
        [ -s "$f" ] && echo "    shot $(basename "$f")" \
                    || echo "    ⚠️  NO PICTURE ($(basename "$f"))"
        # DRIVE THE CAMERA BETWEEN SHOTS when the clickpipe is armed.
        # ⚠️ Without input, "the picture did not change" proves NOTHING. A map
        # with no level script has no AI and no animation, so if nobody touches
        # the mouse a live renderer legitimately presents pixel-identical frames
        # — measured: runs/afg1's t6min and t7min are byte-identical, and t7 vs
        # t8 differ ONLY in the title bar's fps counters (0 of 924,160 body
        # pixels). That is indistinguishable from a frozen presenter until you
        # move the view. So move it, and alternate so it stays near where it was.
        if [ -n "${BO2_CLICKPIPE:-}" ] && [ -p "${BO2_CLICKPIPE}" ]; then
            if [ $(( i % 2 )) -eq 0 ]; then MX=0.35; else MX=0.65; fi
            ./scripts/sendclick "$BO2_CLICKPIPE" move "$MX" 0.5 >/dev/null 2>&1 || true
        fi
        sleep 3
    done
    echo "  done photographing; leaving the run to continue"
    # ONE render-target dump, and only if the caller armed it. SIGUSR1 is the
    # trigger for both the shot and the RT dump; shot.sh does NOT use it (it
    # captures by CoreGraphics window id), so the twelve shots above did not
    # fire this and the dump lands exactly once, on a level that is up.
    # Opt-in because it writes ~13 PNGs and instruments perturb (GUIDE rule 7).
    if [ -n "${BO2_RT_DUMP:-}" ]; then
        echo "  *** SIGUSR1 -> render-target dump to ${BO2_RT_DUMP}* ***"
        kill -USR1 "$PID" 2>/dev/null && sleep 6
        ls -la "${BO2_RT_DUMP}"* 2>/dev/null | tail -20 \
            || echo "    ⚠️  NO RT DUMP FILES — the dump did not run"
    fi
}

FIRED=0
PREV_DRAWS=$(draw_lines)
PREV_T=$(date +%s)
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    kill -0 "$PID" 2>/dev/null || { echo "  process exited"; break; }
    if [ "$FIRED" -eq 0 ]; then
        # Trigger 1 — the engine's own "player is at the spawn point" event.
        if grep -q '\[SETORIGIN\]' "$LOG" 2>/dev/null; then
            FIRED=1; photograph "[SETORIGIN] seen — the player is placed"
        else
            # Trigger 2 — the draw RATE, sampled over >=10 s. The menu emits
            # roughly one [D3D-DRAW] line every 27 s; a level emits ~23 PER
            # SECOND. Any threshold in between separates them; 50 lines in a
            # 10 s window is two orders of magnitude clear of the menu.
            NOW=$(date +%s)
            if [ $(( NOW - PREV_T )) -ge 10 ]; then
                CUR=$(draw_lines)
                if [ $(( CUR - PREV_DRAWS )) -ge 50 ]; then
                    FIRED=1
                    photograph "draw rate $(( (CUR - PREV_DRAWS) * 1000 / (NOW - PREV_T) ))k draws/s — the world is rendering"
                fi
                PREV_DRAWS=$CUR; PREV_T=$NOW
            fi
        fi
    fi
    sleep 2
done

# NEVER END WITH NOTHING. A run whose trigger never fired still has to say what
# was on the screen, or the next session inherits the same blind spot.
if [ "$FIRED" -eq 0 ] && kill -0 "$PID" 2>/dev/null; then
    echo "  ⚠️  NEITHER TRIGGER FIRED in ${MINUTES} min — photographing anyway so"
    echo "      this run is not another empty one. Treat these as 'whatever was"
    echo "      on screen at the deadline', NOT as pictures of a loaded level."
    photograph "deadline reached, no trigger"
fi

kill "$PID" 2>/dev/null; sleep 1; kill -9 "$PID" 2>/dev/null
echo "--- summary ---"
grep -c '' "$LOG" | sed 's/^/  log lines: /'
for pat in '\[SETORIGIN\]' 'exceeded maximum' '\[RUNAWAY\]' 'CRASH (signal' '\[CLICKPIPE\]' '\[D3D-PRESENT\]'; do
    printf "  %-22s %s\n" "$pat" "$(grep -c "$pat" "$LOG" 2>/dev/null || echo 0)"
done
ls -la "$OUT"/level_*.png 2>/dev/null | tail -12 || echo "  (no level shots — did it reach [SETORIGIN]?)"

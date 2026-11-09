#!/usr/bin/env bash
# scripts/harness/run.sh [game dir] — start the recompiled game on the PC (WSL), headless, in the background.
#
# Runs android/build/harness/bo2_harness (scripts/package-game.sh with HARNESS=1) under qemu-user, with the same
# environment the app sets (android/app/.../Bo2App.java: the game folder, and bo2-mac's play_steam.sh zm settings).
# The game folder is a copy of the player's data (scripts/harness/README.md): the game writes into players/.
# Drive it with scripts/harness/ctl.sh; the log is $RUN/log.txt.
set -euo pipefail
APK="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${1:-$HOME/bo2-gamedir}"
RUN="${HARNESS_RUN:-$HOME/harness-run}"
EXE="${HARNESS_EXE:-$APK/android/build/harness/bo2_harness}"   # HARNESS_EXE: another build (built while one runs)
[ -x "$EXE" ] || { echo "no $EXE: build it with HARNESS=1 scripts/package-game.sh" >&2; exit 1; }
[ -f "$GAME/pe_sections.bin" ] && [ -d "$GAME/zone" ] || { echo "$GAME is not a prepared game folder" >&2; exit 1; }
# Any harness, whichever build: they all share the control FIFO, so a leftover would take button presses.
q=qemu-aarch64; pkill -f "$q .*bo2_""harness" 2> /dev/null || true; sleep 1
mkdir -p "$RUN/home/shader_cache" "$RUN/shots"
rm -f "$RUN/ctl"
[ -f "$RUN/log.txt" ] && mv -f "$RUN/log.txt" "$RUN/log.prev.txt"   # the previous run's log, kept for comparison
export BO2_GAME_DIR="$GAME" BO2_PE_SECTIONS="$GAME/pe_sections.bin" HOME="$RUN/home" \
    BO2_SHADER_CACHE_DIR="$RUN/home/shader_cache" BO2_REFUSAL_FILE="$RUN/refusal.txt" BO2_LANG=en \
    BO2_HARNESS_CTL="$RUN/ctl"
# bo2-mac's scripts/play_steam.sh zm, as Bo2App sets them.
export BO2_WINDOW=1 BO2_STALL_ABORT=0 BO2_MAX_IMPORTS=0 BO2_FORCE_FOCUS=1 BO2_MUTE=0 BO2_XAUDIO=1 BO2_KB_PHYSICAL=1 \
    BO2_DEPTH_BIAS=1 BO2_DEPTH_DSV=1 BO2_SV_INLINE=1 BO2_NO_FORCE_MAP=1 BO2_MSGB=150 BO2_FFOTD_TU=13 \
    BO2_VB_RESIDENT=1 BO2_VPX_NATIVE=1 BO2_VPX_REPLACE=1 BO2_W32_FAST=1 BO2_PREEMPT_W=20000 BO2_CS_COOP=1 \
    BO2_RR_FAIR=1 BO2_REAL_MUTEX=1 BO2_HOST_THREADS=jq,snd,snddec,srv,backend BO2_HW_ATOMICS=2 BO2_SHADER_CREE=1 \
    BO2_CMDPIPE="$RUN/home/bo2zms.cmd"
# Extra switches for one run: HARNESS_ENV="NAME=VALUE ...".
for kv in ${HARNESS_ENV:-}; do export "$kv"; done
nohup "$EXE" > "$RUN/log.txt" 2>&1 &
echo "$!" > "$RUN/pid"
echo "harness pid $(cat "$RUN/pid"), log $RUN/log.txt, control $RUN/ctl"

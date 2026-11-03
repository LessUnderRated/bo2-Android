#!/bin/bash
# launch.sh <campagne|zombies|multi> [BO2_X=…] — THE launcher of the three modes, each at its main menu.
# The game's menus switch from one mode to another (CAMPAIGN / ZOMBIES / MULTIPLAYER -> ShellExecuteA("steam://run/…")):
# the runtime's relay (bo2_relais_jeu) waits for the current game to end, then calls this script back with that mode.
#   BO2_NAME: player name on screen (default: the profile's).
# The relay only passes on BO2_NAME and MTL_HUD_ENABLED: the rest of one game's environment does not
# suit the other (P-Z-NATIF-16j: the campaign stayed black with the Zombies environment).
cd "$(dirname "$0")/.." || exit 1
exec >> "$HOME/Library/Logs/BlackOpsII.log" 2>&1   # the relay discards its output: a launcher refusal shows up here
echo "--- $(date +%H:%M:%S) launch.sh $*"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"   # launched by Finder (.app): minimal PATH, no python3
# Launched by Finder, the game only gets 256 open files (launchctl limit maxfiles) and holds ~290 of them at the
# menu: BO2_SHOT captures "FAILED", silent relay to another mode (Too many open files, 10/24). The terminal gives 1 M.
ulimit -n "$(/usr/sbin/sysctl -n kern.maxfilesperproc)" 2>/dev/null || ulimit -n 10240
echo "open files: $(ulimit -n) max"
MODE=${1:-campagne}; shift
NOM=${BO2_NAME:+BO2_NAME=$BO2_NAME}
case "$MODE:${BO2_PANNEAU:-0}" in
  campagne:*) exec scripts/play_steam.sh sp ${NOM:+"$NOM"} "$@" ;;
  zombies:*)  exec scripts/play_steam.sh zm ${NOM:+"$NOM"} "$@" ;;
  multi:*)    exec scripts/play_steam.sh mp ${NOM:+"$NOM"} "$@" ;;
  *) echo "unknown mode: $MODE (campagne, zombies or multi)" >&2; exit 2 ;;
esac

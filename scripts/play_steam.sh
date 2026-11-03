#!/bin/bash
# play_steam.sh zm|mp|sp [BO2_X=…] — JOUER sur l'exécutable Steam, sans minuteur. Contrôles :
# ZQSD par position, visée à la souris ou au trackpad (pointeur tenu après un
# clic dans la fenêtre, ⌘ le rend), son actif. Arrêt : fermer la fenêtre.
set -u
cd "$(dirname "$0")/.."
case "${1:-}" in
  zm) T=zms; TU=13 ;;
  mp) T=mps; TU=17 ;;
  sp) T=sps; TU= ;;
  *) echo "usage: scripts/play_steam.sh zm|mp|sp [BO2_X=…]"; exit 1 ;;
esac
M=$1; shift
[ -x boot_cache/$T/boot ] || {   # converted after the campaign: the conversion resumes when the app opens again
  M="This mode is still being converted. Open Black Ops II again to resume the conversion."
  [ "$(defaults read -g AppleLanguages 2>/dev/null | tr -dc 'a-zA-Z-\n' | sed -n 2p | cut -c1-2)" = fr ] \
    && M="Ce mode est encore en conversion. Rouvrez Black Ops II pour la reprendre."
  osascript -e "display alert \"Black Ops II\" message \"$M\"" >/dev/null 2>&1; echo "REFUS: $T pas encore converti"; exit 1; }
GAME="$(cat ~/.bo2-native/game_dir 2>/dev/null || cat ~/.bo2-native/jeu)"
# Touches de déplacement par position (ZQSD en AZERTY sur les W/A/S/D par défaut), sauf si le joueur a lié « avancer »
# à Z dans ce mode, comme un joueur AZERTY sous Windows : alors par lettre, comme sous Windows.
KB=1; grep -qs '^bind Z "+forward"' "$GAME/players/bindings_$M.bdg" && KB=0
OUT=runs/jouer-$T-$(date +%Y%m%d_%H%M%S); mkdir -p "$OUT"; LOG="$OUT/run.log"
pgrep -x boot >/dev/null && { echo "REFUS: boot actif"; exit 1; }
[ "$(df -g . | awk 'NR==2 {print $4}')" -ge 8 ] || { echo "REFUS: moins de 8 GiB libres"; exit 1; }
cp -R "$GAME/players" "$OUT/players_before"   # le jeu écrit la vraie sauvegarde
scripts/msl_cache.sh "$PWD/boot_cache/$T/msl_cache"   # shaders des parties précédentes -> metallib (sinon chaque partie les recompile, gels de plusieurs s)
rm -f /tmp/bo2$T.cmd /tmp/bo2$T.key
[ "$T" = sps ] && set -- BO2_MIP_CIBLES=0 "$@"   # P856 : sans ça, fumée fausse en campagne
[ "$T" = sps ] || set -- BO2_SHADER_CREE=1 "$@"   # P1111 : shaders traduits au chargement ; pas en campagne, la cinématique de chargement tomberait à 1 img/s
env DYLD_LIBRARY_PATH="$PWD/boot_cache/hlslcc_build" \
    BO2_WINDOW=1 BO2_STALL_ABORT=0 BO2_MAX_IMPORTS=0 BO2_FORCE_FOCUS=1 BO2_MUTE="${BO2_MUTE:-0}" BO2_XAUDIO=1 BO2_KB_PHYSICAL=$KB \
    BO2_DEPTH_BIAS=1 BO2_DEPTH_DSV=1 BO2_SV_INLINE=1 BO2_NO_FORCE_MAP=1 BO2_MSGB=150 \
    ${TU:+BO2_FFOTD_TU=$TU} BO2_VB_RESIDENT=1 BO2_VPX_NATIVE=1 BO2_VPX_REPLACE=1 \
    BO2_W32_FAST=1 BO2_PREEMPT_W=20000 BO2_CS_COOP=1 BO2_RR_FAIR=1 BO2_REAL_MUTEX=1 \
    BO2_HOST_THREADS=jq,snd,snddec,srv,backend BO2_HW_ATOMICS=2 BO2_CMDPIPE=/tmp/bo2$T.cmd BO2_KEYPIPE=/tmp/bo2$T.key \
    "$@" ./boot_cache/$T/boot boot_cache/$T/pe_sections.bin >> "$LOG" 2>&1 &
PID=$!
caffeinate -dimsu -w "$PID" >/dev/null 2>&1 &
echo "[CAFFEINE] caffeinate -dimsu -w $PID" >> "$LOG"
echo "$PID $OUT"

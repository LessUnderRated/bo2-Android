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
[ -x boot_cache/$T/boot ] || {   # not converted yet: offer it, then reopen the app, which converts up to this mode (issue #3)
  case "$(defaults read -g AppleLanguages 2>/dev/null | tr -dc 'a-zA-Z-\n' | sed -n 2p | cut -c1-2)" in
    fr) Q="Ce mode n'est pas encore converti (jusqu'à 1 h 30 sur un M1). Le convertir maintenant ?"; O=Convertir; N="Plus tard";;
    de) Q="Dieser Modus ist noch nicht umgewandelt (bis zu 1,5 Std. auf einem M1). Jetzt umwandeln?"; O=Umwandeln; N="Später";;
    es) Q="Este modo aún no está convertido (hasta 1 h 30 en un M1). ¿Convertirlo ahora?"; O=Convertir; N="Más tarde";;
    it) Q="Questa modalità non è ancora convertita (fino a 1 h 30 su un M1). Convertirla ora?"; O=Converti; N="Più tardi";;
    pt) Q="Este modo ainda não foi convertido (até 1 h 30 num M1). Convertê-lo agora?"; O=Converter; N="Mais tarde";;
    pl) Q="Ten tryb nie jest jeszcze przekonwertowany (do 1,5 h na M1). Przekonwertować teraz?"; O=Konwertuj; N="Później";;
    ru) Q="Этот режим ещё не преобразован (до 1,5 ч на M1). Преобразовать сейчас?"; O="Преобразовать"; N="Позже";;
    ja) Q="このモードはまだ変換されていません（M1で最大1時間30分）。今すぐ変換しますか？"; O="変換"; N="後で";;
    *)  Q="This mode isn't converted yet (up to 1.5 h on an M1). Convert it now?"; O=Convert; N="Not now";;
  esac
  R=$(osascript -e "button returned of (display alert \"Black Ops II\" message \"$Q\" buttons {\"$N\", \"$O\"} default button \"$O\")" 2>/dev/null)
  if [ "$R" = "$O" ]; then   # raise the target, never lower it (no file = all three already)
    case "$(cat boot_cache/conversion_target 2>/dev/null)$T" in spszms|spsmps|zmsmps) echo $T > boot_cache/conversion_target ;; esac
    open "$(cat boot_cache/app_path 2>/dev/null)" 2>/dev/null || open -b local.bo2-native.launcher
  fi
  echo "REFUS: $T pas encore converti"; exit 1; }
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

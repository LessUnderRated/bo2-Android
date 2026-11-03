#!/bin/bash
# Régénère runtime/win32/snd_reverb_gen.inc depuis les exe du joueur (la réverbération des trois jeux, traduite
# instruction par instruction par scripts/x86c.py). Le fichier n'est pas distribué : c'est du code du jeu.
set -e
cd "$(dirname "$0")/.."
[ -n "${BO2_REVERB_STEAM:-}" ] || {
GAME="${BO2_GAME_DIR:-$(cat ~/.bo2-native/game_dir 2>/dev/null || cat ~/.bo2-native/jeu 2>/dev/null)}"
[ -d "$GAME" ] || { echo "game folder not found: set BO2_GAME_DIR or run scripts/install_steam.sh (~/.bo2-native/game_dir)"; exit 1; }
}
X=scripts/x86c.py
OUT=${OUT:-runtime/win32/snd_reverb_gen.inc}
if [ -n "${BO2_REVERB_STEAM:-}" ]; then   # mêmes corps sur l'exe Steam (tools/steam/*_redirects.tsv)
    S=~/.bo2-native/steam
    { python3 $X $S/t6mp.exe.unpacked.exe 83b1a0 83bdc4 bo2_snd_reverb_mp SND-REV
      python3 $X $S/t6zm.exe.unpacked.exe 83c170 83cd94 bo2_snd_reverb_zm SND-REV
      python3 $X $S/t6sp.exe.unpacked.exe 806290 806eb4 bo2_snd_reverb_sp SND-REV; } > $OUT.tmp
fi
mv $OUT.tmp $OUT
echo "$OUT : $(wc -l < $OUT) lignes"

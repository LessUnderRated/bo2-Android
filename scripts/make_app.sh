#!/bin/bash
# make_app.sh [folder] — builds "Black Ops II.app" (default: /Applications): double-click = the campaign at the main
# menu, no panel, the way Steam launches t6sp; ZOMBIES / MULTIPLAYER are launched from the game's own menus
# (bo2_relais_jeu relay -> scripts/launch.sh). The icon is read from the user's t6sp.exe, never stored here.
# No Metal HUD and no counters in the title (MTL_HUD_ENABLED=0, BO2_TITRE=0): the test launchers keep them.
# Launcher log: ~/Library/Logs/BlackOpsII.log.
#
# The bundle carries the sources only (Contents/Resources/bo2), no byte taken from the game. On first launch it asks
# for the game folder, then install_steam.sh and build_steam.sh translate the player's own Steam executables into
# ~/.bo2-native/app-local, behind the preparation window (tools/setup_window.swift). The bundle stays read-only, so its
# signature holds. `--from-source` is accepted for compatibility (it is the only mode).
set -eu
ICI="$(cd "$(dirname "$0")/.." && pwd)"
[ "${1:-}" = --from-source ] && shift
DEST="${1:-/Applications}"
APP="$DEST/${BO2_APP_NAME:-Black Ops II}.app"   # BO2_APP_NAME: another bundle name
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Black Ops II</string>
  <key>CFBundleDisplayName</key><string>Black Ops II</string>
  <key>CFBundleIdentifier</key><string>local.bo2-native.launcher</string>
  <key>CFBundleExecutable</key><string>BlackOpsII</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>2.0</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
EOF

# The mini-repository: same relative paths as in the repository, so the launchers and the relay need not know.
B="$APP/Contents/Resources/bo2"
mkdir -p "$B"
rsync -a --exclude .git --exclude boot_cache --exclude runs --exclude bin --exclude 'tools/*-src' "$ICI/" "$B/"
swiftc -O -o "$APP/Contents/MacOS/preparation" "$ICI/tools/setup_window.swift"
touch "$APP/Contents/Resources/from-source"
echo "$(date '+%Y-%m-%d %H:%M:%S') $(git -C "$ICI" rev-parse --short HEAD)" > "$APP/Contents/Resources/version"

cat > "$APP/Contents/MacOS/BlackOpsII" <<'EOF'
#!/bin/bash
# Clones Resources/bo2 into ~/.bo2-native/app-local when the version changes (the working copy, where the game writes), then launches.
R="$(cd "$(dirname "$0")/../Resources" && pwd)"
W="$HOME/.bo2-native/app-local"
exec >> "$HOME/Library/Logs/BlackOpsII.log" 2>&1
export PATH="$PATH:/opt/homebrew/bin"   # launched by Finder: no Homebrew cmake/ninja; appended, so python3 stays the one that passed the capstone check
echo "--- $(date +%H:%M:%S) bundle $(cat "$R/version")"
case "$(defaults read -g AppleLanguages 2>/dev/null | tr -dc 'a-zA-Z-\n' | sed -n 2p | cut -c1-2)" in   # folder prompt, no exe, copy failed
  fr) T1="Choisissez le dossier de Black Ops II (celui qui contient t6sp.exe)"; T2="Aucun t6sp.exe dans"; T3="Copie impossible vers";;
  de) T1="Wählen Sie den Ordner von Black Ops II (der t6sp.exe enthält)"; T2="Keine t6sp.exe in"; T3="Kopieren nicht möglich nach";;
  es) T1="Elija la carpeta de Black Ops II (la que contiene t6sp.exe)"; T2="No hay t6sp.exe en"; T3="No se puede copiar a";;
  it) T1="Scelga la cartella di Black Ops II (quella che contiene t6sp.exe)"; T2="Nessun t6sp.exe in"; T3="Impossibile copiare in";;
  pt) T1="Escolha a pasta do Black Ops II (a que contém o t6sp.exe)"; T2="Nenhum t6sp.exe em"; T3="Não foi possível copiar para";;
  pl) T1="Folder Black Ops II (zawierający t6sp.exe)"; T2="Brak t6sp.exe w"; T3="Nie można skopiować do";;
  ru) T1="Выберите папку Black Ops II (в которой лежит t6sp.exe)"; T2="Нет t6sp.exe в"; T3="Не удалось скопировать в";;
  ja) T1="Black Ops IIのフォルダ（t6sp.exeがあるフォルダ）を選択してください"; T2="t6sp.exeが見つかりません:"; T3="コピーできません:";;
  *)  T1="Choose the Black Ops II folder (the one containing t6sp.exe)"; T2="No t6sp.exe in"; T3="Cannot copy to";;
esac
alerte() { osascript -e "display alert \"Black Ops II\" message \"$1\"" >/dev/null; exit 1; }
G="$HOME/.bo2-native/game_dir"; [ -s "$G" ] || [ ! -s "$HOME/.bo2-native/jeu" ] || G="$HOME/.bo2-native/jeu"   # older name
if [ ! -s "$G" ]; then
  J=$(osascript -e "POSIX path of (choose folder with prompt \"$T1\")") || exit 1
  J="${J%/}"; [ -f "$J/t6sp.exe" ] || alerte "$T2 $J"
  mkdir -p "$HOME/.bo2-native" && printf '%s\n' "$J" > "$G"
fi
E=$(cat "$W/preparation.pid" 2>/dev/null) || true   # a conversion running: its sources stay, the copy waits for the next launch
if ! cmp -s "$R/version" "$W/.version" && { [ -z "$E" ] || ! kill -0 "$E" 2>/dev/null; }; then
  echo "working copy -> $W"
  mkdir -p "$W" && ditto --clone "$R/bo2" "$W" && cp "$R/version" "$W/.version" || alerte "$T3 $W"
fi
mkdir -p "$W/boot_cache" && echo "${R%/Contents/Resources}" > "$W/boot_cache/app_path"   # reopened by play_steam.sh
C=$(cat "$W/boot_cache/conversion_target" 2>/dev/null); case "$C" in sps|zms) ;; *) C=mps ;; esac   # the modes asked for (issue #3)
# First launch: translate the player's exes. After an update, again what its sources changed (scripts/signature.sh).
if [ -f "$R/from-source" ] && { [ ! -x "$W/boot_cache/$C/boot" ] || ! bash "$W/scripts/signature.sh" check; }; then
  [ -s "$W/icon.png" ] || { python3 "$W/tools/exe_icon.py" "$(cat "$G")/t6sp.exe" "$W/icon.ico" \
    && sips -s format png "$W/icon.ico" --out "$W/icon.png" >/dev/null && bash "$W/scripts/icon_margin.sh" "$W/icon.png"; } || true
  while :; do
    P=$(cat "$W/preparation.pid" 2>/dev/null) || true
    if [ -z "$P" ] || ! kill -0 "$P" 2>/dev/null; then            # a preparation already running: just show it
      (cd "$W" && mkdir -p boot_cache && bash scripts/install_steam.sh "$(cat "$G")" \
         && bash scripts/build_steam.sh) > "$W/preparation.log" 2>&1 &
      P=$!; echo $P > "$W/preparation.pid"
      caffeinate -i -w "$P" &                                     # no sleep while the preparation runs
    fi
    "$(dirname "$0")/preparation" "$W/preparation.log" "$P"; rc=$?
    [ $rc = 3 ] || break                                          # 3 = "Try Again" (build_steam.sh resumes)
  done
  if [ $rc = 4 ]; then                                            # 4 = play the campaign now: stop the whole conversion tree
    echo sps > "$W/boot_cache/conversion_target"                  # and keep the other modes for when they are picked
    tue() { kill -STOP "$1" 2>/dev/null; for c in $(pgrep -P "$1"); do tue "$c"; done; kill -KILL "$1" 2>/dev/null; }
    trap '' HUP; tue "$P"; trap - HUP   # launchd's child = orphaned group: a death beside stopped members HUPs it all, us too
    cd "$W" && exec env BO2_PANNEAU=0 MTL_HUD_ENABLED=0 BO2_TITRE=0 scripts/launch.sh campagne
  fi
  [ $rc = 0 ] && [ -x "$W/boot_cache/$C/boot" ] || exit 0           # window closed: the preparation goes on without it
fi
cd "$W" && exec env BO2_PANNEAU=0 MTL_HUD_ENABLED=0 BO2_TITRE=0 scripts/launch.sh campagne
EOF
chmod +x "$APP/Contents/MacOS/BlackOpsII"

codesign -s - --force "$APP" 2>/dev/null || true   # ad-hoc signature: Gatekeeper does not open an unsigned bundle
touch "$APP"
echo "$APP"

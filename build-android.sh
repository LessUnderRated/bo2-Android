#!/usr/bin/env bash
# build-android.sh --steam "<your Black Ops II folder>" [--push-data] [--no-install] [--lift]
#
# Builds the debug APK on this PC (Linux or WSL2) and installs it on the phone over adb.
#
#   --steam DIR   your own Steam install of Black Ops II (steamapps/common/Call of Duty Black Ops II).
#                 Checked to be a BO2 install; nothing in it is copied into this repository or the APK.
#   --push-data   also copy the game's data to the phone over USB (adb push), into the app's
#                 external files directory. Much faster than importing in the app (SAF).
#   --no-install  build only.
#   --lift        recompile Zombies from DIR/t6zm.exe.unpacked.exe (your own SteamStub-unwrapped exe): bo2-mac's
#                 pipeline retargeted to Android, then the game core (android/build/game/libbo2core.so). Linux or WSL.
#
# The APK never contains the game: it is the app, the renderer and the shell (touch controls, controller, device report,
# data import, logs, a diagnostics screen). The game core built by --lift is copied to the phone's game folder with the
# install (or put it in the folder you import); the app loads it from there. It stays on this PC and your phone.
set -euo pipefail
cd "$(dirname "$0")"

STEAM=""
PUSH=0
INSTALL=1
LIFT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --steam) STEAM="${2:?--steam needs a folder}"; shift 2 ;;
        --push-data) PUSH=1; shift ;;
        --no-install) INSTALL=0; shift ;;
        --lift) LIFT=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
done

die() { echo "build-android.sh: $*" >&2; exit 1; }
[ "$PUSH" = 0 ] || [ -n "$STEAM" ] || die "--push-data needs --steam"
[ "$PUSH" = 0 ] || [ "$INSTALL" = 1 ] || die "--push-data needs the phone: drop --no-install"
case "${GRADLE_BIN:-}" in ""|/*) ;; *) GRADLE_BIN="$PWD/$GRADLE_BIN" ;; esac
PKG=io.github.lessunderrated.bo2

# ---- Toolchain -----------------------------------------------------------------------------------
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
[ -d "$SDK" ] || die "no Android SDK at $SDK: install the command-line tools and set ANDROID_HOME"
missing=()
while read -r pkg; do
    [ -z "$pkg" ] && continue
    [ -d "$SDK/$(echo "$pkg" | tr ';' '/')" ] || missing+=("$pkg")
done < android/packages.txt
if [ ${#missing[@]} -gt 0 ]; then
    die "missing SDK packages; install them with:
  $SDK/cmdline-tools/latest/bin/sdkmanager $(printf '"%s" ' "${missing[@]}")"
fi
command -v java > /dev/null || die "no JDK: install JDK 17 or later"
command -v git > /dev/null || die "no git"
ADB="$SDK/platform-tools/adb"

# ---- The player's game folder --------------------------------------------------------------------
if [ -n "$STEAM" ]; then
    [ -d "$STEAM/zone" ] || die "$STEAM has no zone/ folder: is it the Black Ops II install folder?"
    found=()
    for exe in t6zm.exe t6mp.exe t6sp.exe; do [ -f "$STEAM/$exe" ] && found+=("$exe"); done
    [ ${#found[@]} -gt 0 ] || die "$STEAM has none of t6zm.exe, t6mp.exe, t6sp.exe"
    echo "game folder: $STEAM (${found[*]})"
fi

GAME=android/build/game
if [ "$LIFT" = 1 ]; then
    # The recompile (docs/LIFT.md): bo2-mac's pipeline for Zombies, retargeted to Android, from the player's own
    # unwrapped exe. Its results are derived from the game and stay on this machine (android/build is ignored).
    [ -n "$STEAM" ] || die "--lift needs --steam"
    [ -f "$STEAM/t6zm.exe.unpacked.exe" ] || die "--lift needs $STEAM/t6zm.exe.unpacked.exe: your t6zm.exe with Valve's
SteamStub wrapper removed, made by you (this repository provides no way to do that)"
    [ "$(uname -s)" = Linux ] || die "--lift runs on Linux or WSL"
    LNDK="${ANDROID_NDK_LINUX:-$SDK/ndk/$(sed -n 's/^ndk;//p' android/packages.txt)}"
    [ -d "$LNDK/toolchains/llvm/prebuilt/linux-x86_64" ] || die "no Linux NDK at $LNDK (set ANDROID_NDK_LINUX)"
    mkdir -p ~/.bo2-native/steam
    ln -sf "$STEAM/t6zm.exe.unpacked.exe" ~/.bo2-native/steam/t6zm.exe.unpacked.exe
    printf '%s\n' "$STEAM" > ~/.bo2-native/game_dir
    scripts/upstream.sh
    BO2="${BO2_LIFT_DIR:-$HOME/bo2-lift}"
    [ -d "$BO2" ] || cp -r android/build/upstream/bo2-mac "$BO2"   # a writable copy: the lift builds inside it
    scripts/lift-android.sh "$BO2"
    scripts/build-deps-android.sh "$LNDK" android/build/deps-arm64
    scripts/package-game.sh "$BO2" "$LNDK" android/build/deps-arm64 "$GAME"
fi

# ---- The APK -------------------------------------------------------------------------------------
[ -f android/local.properties ] || echo "sdk.dir=$SDK" > android/local.properties
GRADLE=(./gradlew)
if [ -n "${GRADLE_BIN:-}" ]; then GRADLE=("$GRADLE_BIN"); fi
(cd android && "${GRADLE[@]}" --no-daemon assembleDebug)
APK=android/app/build/outputs/apk/debug/app-debug.apk
[ -f "$APK" ] || die "no APK at $APK"
echo "built $APK"

[ "$INSTALL" = 1 ] || exit 0
"$ADB" get-state > /dev/null 2>&1 || die "no single phone on adb: enable USB debugging and connect it, or pick one with ANDROID_SERIAL=<serial> (the APK is at $APK)"
"$ADB" install -r "$APK"
if [ -f "$GAME/libbo2core.so" ]; then
    "$ADB" push "$GAME/libbo2core.so" /sdcard/Android/data/io.github.lessunderrated.bo2/files/game/libbo2core.so
    echo "game core pushed to the phone's game folder (yours only: it is made from your own copy)"
fi

if [ "$PUSH" = 1 ]; then
    DEST="/sdcard/Android/data/$PKG/files/game"
    "$ADB" shell mkdir -p "$DEST"
    # The mark says "whole import": it goes first and comes back only once every push succeeded.
    "$ADB" shell rm -f "$DEST/.bo2-import-complete"
    # Everything but Steam's redistributables. adb push resumes nothing: a stopped push starts again.
    for entry in "$STEAM"/*; do
        name=$(basename "$entry")
        case "$name" in redist|__CommonRedist|_CommonRedist) continue ;; esac
        # Your SteamStub-unwrapped exes are inputs of the lift on this PC, not game data: they stay off the phone.
        case "$name" in *.unpacked.exe) echo "skipping $name (lift input, stays on this PC)"; continue ;; esac
        echo "pushing $name"
        "$ADB" push "$entry" "$DEST/" > /dev/null
    done
    # The recompiled game's PE sections (from your exe, made by --lift) go with your data, never into the APK.
    if [ -f "$GAME/pe_sections.bin" ]; then echo "pushing pe_sections.bin"; "$ADB" push "$GAME/pe_sections.bin" "$DEST/" > /dev/null; fi
    # adb creates the folders as `shell` with mode 2770, which the app's own user cannot enter (Android 17 on the S26).
    "$ADB" shell chmod -R a+rwX "$DEST"
    "$ADB" shell touch "$DEST/.bo2-import-complete"
    "$ADB" shell chmod a+rw "$DEST/.bo2-import-complete"
    echo "game data pushed to $DEST"
fi
"$ADB" shell am start -n "$PKG/.SetupActivity" > /dev/null
echo "started. Logs: $ADB logcat -s BO2"

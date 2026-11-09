# scripts/env.sh — the machine paths the build and phone scripts use. Sourced, not run.
#
# Every value can be overridden in the environment or in scripts/env.local.sh (not committed), e.g.
#   ADB=/c/Android/platform-tools/adb.exe
#   BO2_WORK=/d/bo2-work        (captures, symbols and published builds; default: work/ in this repo)
# Paths there may be written the Git Bash way (/d/...): the WSL side reads them as /mnt/d/....
# Works from Git Bash on Windows (adb.exe, gradlew.bat) and from WSL/Linux (lift, package, symbolising).

BO2_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[ -f "$BO2_REPO/scripts/env.local.sh" ] && . "$BO2_REPO/scripts/env.local.sh"

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) BO2_ON_WINDOWS=1 ;;
    *) BO2_ON_WINDOWS=0 ;;
esac
# Git Bash rewrites anything that looks like a path, phone paths included (/data/local/tmp -> C:/Program Files/Git/...).
# So no rewriting at all; scripts pass Windows tools relative paths (cd first) instead.
[ "$BO2_ON_WINDOWS" = 1 ] && export MSYS_NO_PATHCONV=1
# ...and with no rewriting, native Windows tools (git.exe, gradlew.bat) cannot read /d/...: the repo as D:/...,
# which bash and Windows tools both accept.
[ "$BO2_ON_WINDOWS" = 1 ] && BO2_REPO=$(cygpath -m "$BO2_REPO")

# A path as WSL sees it: /d/x and D:/x both become /mnt/d/x (a Linux path is returned unchanged).
bo2_wslpath() { echo "$1" | sed -E 's#^/([A-Za-z])/#/mnt/\L\1/#; s#^([A-Za-z]):/#/mnt/\L\1/#'; }
# A path set once in env.local.sh, as this side sees it: Git Bash and WSL both source that file.
bo2_here() { if [ "$BO2_ON_WINDOWS" = 1 ]; then echo "$1"; else bo2_wslpath "$1"; fi; }

# Everything the scripts make outside the build trees: phone captures, archived symbols, published builds.
BO2_WORK=$(bo2_here "${BO2_WORK:-$BO2_REPO/work}")

ADB=$(bo2_here "${ADB:-$(command -v adb.exe || command -v adb || echo adb)}")
BO2_PKG=${BO2_PKG:-io.github.lessunderrated.bo2}
BO2_CAPTURES=$(bo2_here "${BO2_CAPTURES:-$BO2_WORK/captures}")
# The unstripped libmain.so of each APK build, by build id (scripts/build.sh --apk; scripts/phone/symbolize-prof.sh).
BO2_SYMBOLS=$(bo2_here "${BO2_SYMBOLS:-$BO2_WORK/symbols}")
# The unstripped libmain.so of the last APK build (symbols for simpleperf).
BO2_LIBMAIN=${BO2_LIBMAIN:-$(ls -t "$BO2_REPO"/android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so 2>/dev/null | head -1 || true)}
BO2_APK=${BO2_APK:-$BO2_REPO/android/app/build/outputs/apk/debug/app-debug.apk}
# host:port of tools/bo2-logd (the PC on the tailnet); set it in env.local.sh. Empty: the log stays on the phone.
BO2_LOG_SERVER=${BO2_LOG_SERVER:-}
# Where bo2-logd serves builds to the phone (its --apk-dir); scripts/build.sh --publish writes there.
BO2_APK_DIR=$(bo2_here "${BO2_APK_DIR:-$BO2_WORK/bo2-logd/apk}")

# WSL side (lift, fold, package, symbolising). Paths as WSL sees them.
BO2_REPO_WSL=${BO2_REPO_WSL:-$(bo2_wslpath "$BO2_REPO")}
# The lift runs in a WSL clone of this repo (its android branch, pulled from BO2_REPO_WSL before each build stage):
# the player's exe is lifted there and everything derived from it (boot_cache/) stays there, on this machine.
BO2_WSL_CHECKOUT=${BO2_WSL_CHECKOUT:-'~/bo2-android'}
BO2_NDK_WSL=${BO2_NDK_WSL:-'~/sdk/android-ndk-r28c'}
BO2_SIMPLEPERF=${BO2_SIMPLEPERF:-$HOME/sdk/android-ndk-r28c/simpleperf/bin/linux/x86_64/simpleperf}

# Gradle on Windows: extra NAME=VALUE settings for its run, e.g. to keep its caches and temp off a full C: drive:
#   BO2_GRADLE_ENV="GRADLE_USER_HOME=D:\\gradle TEMP=D:\\tmp TMP=D:\\tmp"
# (The Android SDK: ANDROID_HOME, or sdk.dir in android/local.properties.)
BO2_GRADLE_ENV=${BO2_GRADLE_ENV:-}

bo2_say() { echo "[$(basename "$0") $(date +%H:%M:%S)] $*"; }
bo2_die() { bo2_say "FAILED: $*" >&2; exit 1; }
# Run a command line in WSL (from Windows) or directly (already in Linux).
bo2_linux() {
    if [ "$BO2_ON_WINDOWS" = 1 ]; then wsl bash -lc "$1"; else bash -lc "$1"; fi
}

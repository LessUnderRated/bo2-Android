#!/usr/bin/env bash
# scripts/build.sh [stages] — the phone build, stage by stage, from Git Bash on Windows (or Linux).
#
#   --relift      pull this repo's android branch into the WSL lift checkout, then lift + fold + native + finish (~9 min)
#   --fold        refold the kept slices only (a fold filter or pass changed; ~5 min; needs a previous --relift)
#                 (BO2_FOLD_PASS=1 before either: fold WITH the Rust dead-flags pass, off by default, for an A/B)
#   --package     the game core, android/build/game/libbo2core.so (package-game.sh; runtime or lift changed)
#   --apk         Gradle: the APK (the app and renderer only; no game core in it)
#   --install     adb install -r on the connected phone, and the game core pushed to its game folder
#   --publish     hand the APK to bo2-logd on this PC: the app offers it on its next launch, over Tailscale
#   --release TAG [--notes FILE]   a private GitHub pre-release on this repo with the APK attached
# No stage given: --package --apk. Stages run in the order above whatever order they are given in.
# Paths come from scripts/env.sh (override in scripts/env.local.sh). Each stage stops the build if it fails.
set -euo pipefail
. "$(dirname "$0")/env.sh"

RELIFT=0 FOLD=0 PACKAGE=0 APK=0 INSTALL=0 PUBLISH=0 TAG="" NOTES=""
[ $# -eq 0 ] && { PACKAGE=1; APK=1; }
while [ $# -gt 0 ]; do
    case "$1" in
        --relift) RELIFT=1 ;; --fold) FOLD=1 ;; --package) PACKAGE=1 ;; --apk) APK=1 ;; --install) INSTALL=1 ;;
        --publish) PUBLISH=1 ;;
        --release) TAG=${2:?--release TAG}; shift ;;
        --notes) NOTES=${2:?--notes FILE}; shift ;;
        -h | --help) sed -n '2,13p' "$0"; exit 0 ;;
        *) bo2_die "unknown stage $1 (see --help)" ;;
    esac
    shift
done

W="$BO2_WSL_CHECKOUT/boot_cache/zms"
# The same paths quoted for the WSL command lines below (bo2_q, env.sh): the repo's path may hold spaces.
QR=$(bo2_q "$BO2_REPO_WSL") QC=$(bo2_q "$BO2_WSL_CHECKOUT") QN=$(bo2_q "$BO2_NDK_WSL") QW=$(bo2_q "$W")
# The WSL lift checkout: a clone of this repo's android branch, made on first use and brought up to date before each
# stage (committed work only: commit before building).
pull_bo2mac() {
    bo2_linux "[ -d $QC/.git ] || git clone -q -b android $QR $QC" ||
        bo2_die "could not clone $BO2_REPO_WSL into $BO2_WSL_CHECKOUT"
    bo2_linux "cd $QC && git pull -q $QR android && git log --oneline -1" ||
        bo2_die "could not pull $BO2_REPO_WSL android into $BO2_WSL_CHECKOUT"
}

if [ "$RELIFT" = 1 ]; then
    pull_bo2mac
    bo2_say "relift (lift, fold, native, finish)"
    bo2_linux "rm -f $QW/.lifted $QW/.folded && BO2_FOLD_PASS=${BO2_FOLD_PASS:-0} bash $QR/scripts/lift-android.sh $QC > \$HOME/lift-android.log 2>&1" ||
        { bo2_linux "tail -5 \$HOME/lift-android.log"; bo2_die "relift (see ~/lift-android.log in WSL)"; }
    bo2_linux "tail -1 \$HOME/lift-android.log"
elif [ "$FOLD" = 1 ]; then
    pull_bo2mac
    bo2_linux "ls $QW/batch_0.bc > /dev/null 2>&1" || bo2_die "no kept slices in $W: run --relift once"
    bo2_say "refold (fold, native, finish)"
    bo2_linux "rm -f $QW/.folded && for s in fold native finish; do BO2_FOLD_PASS=${BO2_FOLD_PASS:-0} bash $QR/scripts/lift-android.sh $QC \$s || exit 1; done > \$HOME/lift-android.log 2>&1" ||
        { bo2_linux "tail -5 \$HOME/lift-android.log"; bo2_die "refold (see ~/lift-android.log in WSL)"; }
fi
if [ "$PACKAGE" = 1 ]; then
    [ "$RELIFT" = 1 ] || [ "$FOLD" = 1 ] || pull_bo2mac
    bo2_say "package the game core"
    bo2_linux "bash $QR/scripts/package-game.sh $QC $QN $QR/android/build/deps-arm64 $QR/android/build/game > \$HOME/package.log 2>&1" ||
        { bo2_linux "grep -E 'error|FAILED' \$HOME/package.log | head; tail -3 \$HOME/package.log"; bo2_die "package (see ~/package.log in WSL)"; }
    bo2_linux "grep -E 'redirects:|x87' \$HOME/package.log | head -3; tail -1 \$HOME/package.log"
    # Which commit this core is (the updater shows it; the tree may have moved on by --publish).
    printf 'commit=%s\n' "$(git -C "$BO2_REPO" rev-parse --short HEAD)" > "$BO2_REPO/android/build/game/core-build.txt"
fi
if [ "$APK" = 1 ]; then
    bo2_say "Gradle"
    [ "$BO2_ON_WINDOWS" = 1 ] || bo2_die "--apk runs from Git Bash on Windows (gradlew.bat)"
    # The APK needs no game core (batteries not included): it is built from this repository's sources alone.
    # Gradle's debug packaging updates the APK in place and leaves the old libmain.so's bytes behind as dead space
    # (round 4's APK was 670 MB for 323 MB of entries): start from no APK, so it is written whole.
    rm -f "$BO2_APK"
    rm -rf "$BO2_REPO/android/app/build/intermediates/apk/debug"
    COMMIT=$(git -C "$BO2_REPO" rev-parse --short HEAD)$([ -n "$(git -C "$BO2_REPO" status --porcelain --untracked-files=no)" ] && echo + || true)
    # A core folded WITH the dead-flags pass (BO2_FOLD_PASS=1 in this same run, off by default) says so: -df.
    if [ "${BO2_FOLD_PASS:-0}" = 1 ] && [ "$FOLD$RELIFT" != 00 ]; then COMMIT="$COMMIT-df"; fi
    ( cd "$BO2_REPO/android" && env $BO2_GRADLE_ENV ./gradlew.bat --no-daemon -q assembleDebug \
        "-Pbo2.logServer=${BO2_LOG_SERVER:-}" "-Pbo2.gitCommit=$COMMIT" \
        "-Pbo2.buildNumber=$(git -C "$BO2_REPO" rev-list --count HEAD)" "-Pbo2.buildTime=$(date +%s)" ) || bo2_die "Gradle"
    ls -la "$BO2_APK" | awk '{print "  " $5 " bytes  " $6 " " $7 " " $8}'
    # The unstripped library, by build id: the phone's [PROF] lines name their build, and
    # scripts/phone/symbolize-prof.sh finds the matching symbols here. The last five builds are kept.
    BO2_LIBMAIN=$(ls -t "$BO2_REPO"/android/app/build/intermediates/cxx/*/*/obj/arm64-v8a/libmain.so 2>/dev/null | head -1 || true)
    BID=$(bo2_linux "$QN/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -n $(bo2_q "$(bo2_wslpath "$BO2_LIBMAIN")") | grep -o '[0-9a-f]\{40\}' | head -1" | tr -d '\r')
    if [ -n "$BID" ]; then
        SYMS="$BO2_SYMBOLS"
        mkdir -p "$SYMS/$BID" && cp -f "$BO2_LIBMAIN" "$SYMS/$BID/libmain.so" && echo "$COMMIT" > "$SYMS/$BID/commit.txt"
        # One path per line (-d '\n'): a symbols folder whose path holds spaces must not be split into words for rm.
        ls -1dt "$SYMS"/*/ 2> /dev/null | tail -n +6 | xargs -r -d '\n' rm -rf --
        echo "  symbols: $SYMS/$BID (commit $COMMIT, log server ${BO2_LOG_SERVER:-none})"
    fi
    # What this APK is, for --publish later (the tree may have moved on by then).
    printf 'commit=%s\nbuild_id=%s\n' "$COMMIT" "$BID" > "$(dirname "$BO2_APK")/bo2-build.txt"
fi
if [ "$INSTALL" = 1 ]; then
    bo2_say "install"
    ( cd "$(dirname "$BO2_APK")" && "$ADB" install -r "$(basename "$BO2_APK")" ) | tail -1
    # The player's game core, built by --package, into the game folder: the app copies it into its own storage at start.
    CORE="$BO2_REPO/android/build/game/libbo2core.so"
    if [ -f "$CORE" ]; then
        ( cd "$(dirname "$CORE")" && "$ADB" push libbo2core.so /sdcard/Android/data/io.github.lessunderrated.bo2/files/game/libbo2core.so ) | tail -1
    fi
fi
if [ "$PUBLISH" = 1 ]; then
    # The phone's updater (Updater.java) asks bo2-logd for latest.txt and downloads latest.apk when its sha256 is not
    # the installed APK's. The APK goes in under a temporary name and is renamed, then latest.txt: the server never
    # offers a half-copied file, and a download already running finishes on the build it started with.
    bo2_say "publish to $BO2_APK_DIR"
    [ -f "$BO2_APK" ] || bo2_die "no APK: run --apk"
    INFO="$(dirname "$BO2_APK")/bo2-build.txt"
    PCOMMIT=$(sed -n 's/^commit=//p' "$INFO" 2> /dev/null)
    PBID=$(sed -n 's/^build_id=//p' "$INFO" 2> /dev/null)
    [ -n "$PCOMMIT" ] || PCOMMIT=$(git -C "$BO2_REPO" rev-parse --short HEAD)?
    SIZE=$(stat -c %s "$BO2_APK")
    SHA=$(sha256sum "$BO2_APK" | cut -c1-64)
    mkdir -p "$BO2_APK_DIR"
    cp -f "$BO2_APK" "$BO2_APK_DIR/latest.apk.tmp"
    for _ in 1 2 3 4 5; do mv -f "$BO2_APK_DIR/latest.apk.tmp" "$BO2_APK_DIR/latest.apk" 2> /dev/null && break; sleep 2; done
    [ ! -e "$BO2_APK_DIR/latest.apk.tmp" ] || bo2_die "could not replace $BO2_APK_DIR/latest.apk (in use?)"
    printf 'commit=%s\nsize=%s\nsha256=%s\nbuild_id=%s\n' "$PCOMMIT" "$SIZE" "$SHA" "${PBID:-?}" > "$BO2_APK_DIR/latest.txt.tmp"
    mv -f "$BO2_APK_DIR/latest.txt.tmp" "$BO2_APK_DIR/latest.txt"
    echo "  published build $PCOMMIT ($SIZE bytes, sha256 ${SHA:0:12}...)"
    # The player's game core (built from their own copy by --package), gzipped beside it for their own phone only: the
    # updater fetches it when the phone has none or another one (core.txt: sha256 and size of the unpacked library).
    CORE="$BO2_REPO/android/build/game/libbo2core.so"
    if [ -f "$CORE" ]; then
        CSHA=$(sha256sum "$CORE" | cut -c1-64)
        if [ "$CSHA" != "$(sed -n 's/^sha256=//p' "$BO2_APK_DIR/core.txt" 2> /dev/null)" ]; then
            CCOMMIT=$(sed -n 's/^commit=//p' "$(dirname "$CORE")/core-build.txt" 2> /dev/null)
            gzip -6 -c "$CORE" > "$BO2_APK_DIR/latest-core.so.gz.tmp"
            for _ in 1 2 3 4 5; do mv -f "$BO2_APK_DIR/latest-core.so.gz.tmp" "$BO2_APK_DIR/latest-core.so.gz" 2> /dev/null && break; sleep 2; done
            [ ! -e "$BO2_APK_DIR/latest-core.so.gz.tmp" ] || bo2_die "could not replace $BO2_APK_DIR/latest-core.so.gz (in use?)"
            printf 'commit=%s\nsize=%s\ngz_size=%s\nsha256=%s\n' "${CCOMMIT:-?}" "$(stat -c %s "$CORE")" \
                "$(stat -c %s "$BO2_APK_DIR/latest-core.so.gz")" "$CSHA" > "$BO2_APK_DIR/core.txt.tmp"
            mv -f "$BO2_APK_DIR/core.txt.tmp" "$BO2_APK_DIR/core.txt"
        fi
        echo "  game core $(sed -n 's/^commit=//p' "$BO2_APK_DIR/core.txt") ($(( $(sed -n 's/^gz_size=//p' "$BO2_APK_DIR/core.txt") >> 20 )) MB gzipped, sha256 ${CSHA:0:12}...)"
    fi
fi
if [ -n "$TAG" ]; then
    bo2_say "release $TAG"
    command -v gh > /dev/null || bo2_die "gh (GitHub CLI) not found"
    mkdir -p "$BO2_WORK"
    STAGED="$BO2_WORK/bo2-Android-$TAG.apk"
    cp -f "$BO2_APK" "$STAGED"
    BRANCH=$(git -C "$BO2_REPO" branch --show-current)
    if [ -n "$NOTES" ]; then N=(--notes-file "$NOTES"); else N=(--notes "Private build $(git -C "$BO2_REPO" log -1 --format=%h) for the owner's own devices."); fi
    # gh.exe on Windows cannot open a Git Bash path (/d/...; MSYS_NO_PATHCONV keeps it from being converted).
    ASSET="$STAGED"; [ "$BO2_ON_WINDOWS" = 1 ] && ASSET=$(cygpath -m "$STAGED")
    ( cd "$BO2_REPO" && gh release create "$TAG" "$ASSET" --target "$BRANCH" --title "$TAG" --prerelease "${N[@]}" )
    rm -f "$STAGED"
fi
bo2_say "done"

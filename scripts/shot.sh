#!/bin/bash
# Capture ONLY the BO2 window, by CoreGraphics window id. NEVER use a full-screen
# screencapture here: if another window is in front it photographs the user's
# documents. -l<windowid> captures that window's own surface, occluded or not.
set -e
SP="$(cd "$(dirname "$0")" && pwd)"
OUT=${1:-/tmp/bo2_shot.png}
[ -x "$SP/winid" ] || clang -framework CoreGraphics -framework Foundation \
                          -o "$SP/winid" "$SP/winid.m"
# ⚠️ SAY WHY THERE IS NO PICTURE. A LOCKED SCREEN makes the WindowServer stop
# publishing the game's window to CGWindowList, so winid finds nothing and this
# script used to fail with a bare non-zero exit — full_run.sh then printed
# "shot: 4_t10s" for a file that did not exist, and a whole run's worth of
# captures silently evaporated. Measured 2026-08-01: the same lock also stops
# CoreAudio starting an output queue, which FREEZES the engine (its clock is the
# audio buffer count) — so between them, a locked Mac looks exactly like a code
# regression. It cost four runs and three innocent suspects before I looked.
if ! WID=$("$SP/winid"); then
    if ioreg -n Root -d1 2>/dev/null | grep -q CGSSessionScreenIsLocked; then
        echo "NO CAPTURE: the screen is LOCKED — the window server does not publish" >&2
        echo "  the game window while locked, so no screenshot is possible at all." >&2
        echo "  (The same lock stops CoreAudio; see the audio fallback in" >&2
        echo "   runtime/audio/coreaudio_out.cpp.) Unlock the Mac and re-run." >&2
    else
        echo "NO CAPTURE: no window owned by \"boot\" is at least 100px tall." >&2
        echo "  Is the game actually running, and did it get past [gfx] Metal window?" >&2
    fi
    exit 1
fi
screencapture -x -o -l"$WID" "$OUT"
# ⚠️ BEST EFFORT, AND BOUNDED BY POLLING — not by `wait`.
# `sips` has hung here and frozen a 15-minute measurement run. The first guard
# used `wait` on a subshell plus a killer subshell; it still stalled, because
# killing the SUBSHELL does not kill the grandchild doing the work, and `wait`'s
# exit semantics under `set -e` made it worse. Poll for the pid instead and kill
# the whole tree. A convenience resize must never stop the thing being measured.
( sips -Z 900 "$OUT" --out "${OUT%.png}_s.png" >/dev/null 2>&1 ) &
_sp=$!
_i=0
while [ $_i -lt 15 ] && kill -0 $_sp 2>/dev/null; do sleep 1; _i=$((_i+1)); done
if kill -0 $_sp 2>/dev/null; then
    pkill -P $_sp 2>/dev/null
    kill -9 $_sp 2>/dev/null
    echo "sips timed out (the full-resolution capture is still on disk)" >&2
fi
echo "window $WID -> $OUT"

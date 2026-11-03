#!/bin/bash
# sendkey.sh <pid> <macKeycode> [holdMs] — deliver one key to the game.
#
# Prefers BO2_KEYPIPE (the FIFO the runtime reads, see runtime/gfx/metal_backend.mm)
# and falls back to scripts/sendkey, the CGEventPostToPid version.
#
# ⚠️ WHY THE FIFO IS THE DEFAULT. CGEventPostToPid delivers the key to the
# process, and the process only turns it into a WM_KEYDOWN if AppKit hands it to
# [NSApp nextEventMatchingMask:] — which an INACTIVE application does not get.
# On macOS 14+ an app cannot be activated from outside (both documented routes
# were measured returning success and doing nothing), so on a Mac somebody is
# using, every scripted keystroke is silently discarded. Four runs died that way
# and NONE of them printed anything, because every probe downstream of the
# message queue is quiet when the queue is empty.
#
# The FIFO writes into the same queue in_translate does, with the same messages
# in the same order, so nothing about the guest's view of input changes.
set -u
PID="${1:?usage: sendkey.sh <pid> <keycode> [holdMs]}"
KC="${2:?usage: sendkey.sh <pid> <keycode> [holdMs]}"
HOLD="${3:-60}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -n "${BO2_KEYPIPE:-}" ] && [ -p "${BO2_KEYPIPE}" ]; then
    # Bounded: a FIFO with no reader would block forever, and a measurement that
    # hangs in its own keystroke is worse than one that reports a miss.
    if printf '%s %s\n' "$KC" "$HOLD" > "$BO2_KEYPIPE" 2>/dev/null & then
        w=$!
        i=0; while [ $i -lt 5 ] && kill -0 $w 2>/dev/null; do sleep 1; i=$((i+1)); done
        if kill -0 $w 2>/dev/null; then
            kill -9 $w 2>/dev/null
            echo "sendkey.sh: WRITE TO $BO2_KEYPIPE BLOCKED — no reader; falling back" >&2
        else
            exit 0
        fi
    fi
fi
exec "$HERE/sendkey" "$PID" "$KC" "$HOLD"

#!/bin/bash
# Relaunch angola until the run reaches the TEARDOWN FREEZE case, then stop and
# leave it running for inspection.
#
# Why this exists: six runs on 2026-08-25 with an identical recipe produced three
# different outcomes — the teardown freeze, a pre-existing audio watchdog crash,
# and never leaving the frontend (svWorker=0). Only the first is worth measuring,
# and it appears in roughly one run in three, so BO2_TEARDOWN_GUARD=4 kept never
# firing. Retrying by hand burns a session; retrying here costs nothing.
#
# ⚠️ The success test requires svWorker > 0. A run that never loads a level also
# reports head=0x0000 count=0, which looks exactly like the teardown draining.
cd "$(dirname "$0")/.."
LOG=runs/play-angola/run.log
for attempt in $(seq 1 "${ATTEMPTS:-8}"); do
    # keep the disk from filling: this machine hard-hung once today from exactly that
    for f in runs/play-angola/run-attempt-*.log; do [ -e "$f" ] && gzip -f "$f"; done
    pkill -f boot_cache/boot 2>/dev/null; sleep 3
    echo "[until_freeze] attempt $attempt/${ATTEMPTS:-8} — free $(df -h / | tail -1 | awk '{print $4}')"
    nohup scripts/play.sh angola BO2_KB_PHYSICAL=1 BO2_UI_DROPLAST=${DROPLAST:-1} BO2_SEARCH_GUARD=1 \
        BO2_MT_NULLGUARD=2 BO2_MT_GUARD=2 BO2_SWEEP=3 BO2_PVAR_GUARD=1 \
        BO2_TEARDOWN_GUARD=4 BO2_ASSET_WAIT_TYPES=25 BO2_ASSET_WAIT_MS=2000 \
        BO2_MT_AUDIT_EVERY=4 ${EXTRA_ENV:-} >/dev/null 2>&1 &
    sleep 45
    frozen=0
    for i in $(seq 1 55); do
        sleep 10
        pgrep -f boot_cache/boot >/dev/null || { echo "[until_freeze]   died/exited at ${i}0s"; break; }
        if /usr/bin/grep -q "TEARDOWN-COUNT" "$LOG" 2>/dev/null; then
            echo "[until_freeze] 🎯 GUARD FIRED on attempt $attempt"; exit 0
        fi
        line=$(/usr/bin/grep "^\[CADENCE\]" "$LOG" 2>/dev/null | tail -1)
        sv=$(echo "$line" | /usr/bin/grep -o 'svWorker=[0-9]*' | /usr/bin/grep -o '[0-9]*')
        d=$(echo "$line" | /usr/bin/grep -o 'imports=[0-9]*(+[0-9]*)' | /usr/bin/grep -o '+[0-9]*' | tr -d '+')
        # the freeze case: a level IS loaded (svWorker>0) and imports have stopped
        if [ "${sv:-0}" -gt 0 ] 2>/dev/null && [ "${d:-1}" = "0" ]; then
            frozen=$((frozen+1))
            [ $frozen -ge 4 ] && { echo "[until_freeze] 🎯 FREEZE CASE reached on attempt $attempt (svWorker=$sv)"; exit 0; }
        else
            frozen=0
        fi
    done
    cp "$LOG" "runs/play-angola/run-attempt-$attempt.log" 2>/dev/null
done
echo "[until_freeze] ❌ no attempt reached the freeze case in ${ATTEMPTS:-8} tries"
exit 1

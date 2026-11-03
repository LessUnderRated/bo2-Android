#!/bin/bash
# Launch straight into a campaign map and photograph the load.
#
# WHY THIS EXISTS SEPARATELY FROM full_run.sh. full_run.sh drives the FRONTEND:
# it waits on `open now:.*LockOutMenu`, presses Return, walks the menus. A
# campaign run hijacks the engine the moment the frontend finishes loading
# (BO2_FORCE_MAP_NOW arms on the engine's own "spmap frontend" returning), so
# the title screen never appears — and full_run.sh then sits in that wait for
# its full 1200s x3 timeout while the interesting part happens unobserved.
#
#   scripts/campaign_run.sh <outdir> [minutes] [extra BO2_* env assignments...]
#
# Defaults to afghanistan (campaign mission 1) for 12 minutes, photographing
# every 60s. No keystrokes are sent: the injection is automatic, so this run
# does not depend on window focus, and BO2_KEYPIPE is not needed.
set -u
OUT="${1:?usage: campaign_run.sh <outdir> [minutes] [VAR=VAL ...]}"; shift
MINUTES="${1:-12}"; [ $# -gt 0 ] && shift
cd "$(dirname "$0")/.."
LOG="$OUT/run.log"

# 🪤 A `VAR=VAL` ARGUMENT IS NOT IN *THIS* SHELL'S ENVIRONMENT. The env line below
# expands ${BO2_LEVEL:-afghanistan} in the PARENT, while `BO2_LEVEL=angola` passed
# as an argument only ever reaches the CHILD — so the documented invocation
#     scripts/campaign_run.sh out 8 BO2_LEVEL=angola
# silently ran AFGHANISTAN, printed `level=afghanistan`, and produced a log whose
# every symptom was then attributed to angola. Cost a whole run. Hoist BO2_LEVEL
# out of the argument list before it is used, and let a real exported BO2_LEVEL
# still win when no argument overrides it.
for a in "$@"; do
    case "$a" in BO2_LEVEL=*) BO2_LEVEL="${a#BO2_LEVEL=}"; export BO2_LEVEL ;; esac
done

if pgrep -f '\./boot_cache/boot ' > /dev/null; then
    echo "REFUSING: another boot_cache/boot is already running:" >&2
    pgrep -fl '\./boot_cache/boot ' >&2
    exit 1
fi
# See full_run.sh: a locked Mac publishes no window, so every capture fails and
# CoreAudio may refuse to start — and the engine's clock is the audio buffer
# count. Log- and sample-based results survive it; pictures do not.
if ioreg -n Root -d1 | grep -q 'CGSSessionScreenIsLocked.*Yes'; then
    echo "⚠️  THE MAC IS LOCKED — every screenshot in this run will fail." >&2
fi
mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/run.log

shot() {
    local name="$1" i=0
    ./scripts/shot.sh "$OUT/$name.png" >"$OUT/.shot.err" 2>&1 &
    local sp=$!
    while [ $i -lt 25 ] && kill -0 $sp 2>/dev/null; do sleep 1; i=$((i+1)); done
    kill -0 $sp 2>/dev/null && { pkill -P $sp 2>/dev/null; kill -9 $sp 2>/dev/null
        pkill -9 screencapture 2>/dev/null; echo "  ⚠️  SHOT TIMEOUT ($name)"; return; }
    # A capture that produced no file must never print "shot:" — full_run.sh
    # printed it for a whole locked-screen run and the run read as complete.
    if [ -s "$OUT/$name.png" ]; then echo "  shot: $name"
    else echo "  ⚠️  NO PICTURE ($name): $(head -1 "$OUT/.shot.err" 2>/dev/null)"; fi
}

# SON PAR DEFAUT (demande du testeur, 2026-10-10 : volume réglé par application côté macOS).
# BO2_MUTE=1 keeps the real CoreAudio device, the real callbacks and the real
# pacing — which matters, because the engine's clock is the audio buffer count —
# and only zeroes the samples.
env DYLD_LIBRARY_PATH="$PWD/boot_cache/hlslcc_build" \
    BO2_STALL_ABORT=0 BO2_SV_INLINE=1 BO2_WINDOW=1 BO2_XAUDIO=1 \
    BO2_SND_UNMUTE=1 BO2_UIX_PROBE=1 BO2_VPX_NATIVE=1 BO2_VPX_REPLACE=1 \
    BO2_MUTE="${BO2_MUTE:-0}" \
    BO2_FORCE_MAP_NOW="${BO2_LEVEL:-afghanistan}" \
    "$@" \
    ./boot_cache/boot boot_cache/pe_sections.bin > "$LOG" 2>&1 &
PID=$!
echo "pid=$PID  log=$LOG  level=${BO2_LEVEL:-afghanistan}  for ${MINUTES} min"

# Progress is measured on the ZONE FILE, not on elapsed time: the whole question
# is whether the 48 MB fastfile is being consumed. A run that "looks busy" while
# the read offset never moves is the failure this harness exists to catch.
for m in $(seq 1 "$MINUTES"); do
    sleep 60
    kill -0 "$PID" 2>/dev/null || { echo "  DIED at ~${m} min"; break; }
    shot "t${m}min"
    LEVEL="${BO2_LEVEL:-afghanistan}" python3 - "$LOG" "$m" <<'PY'
import os, re, sys
log, m = sys.argv[1], sys.argv[2]
level = os.environ.get("LEVEL", "afghanistan")
txt = open(log, errors="replace").read()
lines = txt.splitlines()
zone = [l for l in lines if "CreateFileA" in l and ".ff" in l]

# ⚠️ ANCHOR THE READ COUNT TO THE OPEN. File descriptors are RECYCLED: grepping
# a bare `fd=18` over a whole log picks up reads belonging to whatever used that
# number earlier, and doing exactly that is how a "stalled 1 MB into 48 MB"
# result got published when the true count was zero. Only count ReadFile lines
# that appear AFTER the CreateFileA which opened this zone.
mo = re.search(r"CreateFileA[^\n]*%s\.ff[^\n]*fd=(\d+)" % re.escape(level), txt)
if mo:
    fd = mo.group(1)
    after = txt[mo.end():]
    # 🪤 [ReadFile] IS CAPPED AT 6 LINES PER FD. Counting those lines is how this
    # harness reported "angola.ff: read 6 chunks" for a zone that had actually
    # inflated 8,740 of its 8,744 blocks — and that 6 became the headline blocker
    # in the handoff for four sessions. 6 was the cap printing itself, not a
    # measurement. Prefer [READ-TALLY] (uncapped, every 256 reads) and fall back
    # to the capped lines ONLY with the cap stated out loud next to the number.
    tal = re.findall(r"\[READ-TALLY\] fd=%s reads=(\d+) bytes=(\d+)" % fd, after)
    offs = [int(x, 16)
            for x in re.findall(r"\[ReadFile\] fd=%s[^\n]*foff=0x([0-9a-f]+)" % fd, after)]
    # The zone is decompressed by the shim's own inflate, so its tally is a
    # second, independent witness that the zone is being consumed.
    infl = re.findall(r"\[INFLATE-TALLY\] calls=(\d+) produced=(\d+) bytes", after)
    if tal:
        zs = "read %s chunks, %.1f MB" % (tal[-1][0], int(tal[-1][1]) / 1048576.0)
    elif offs:
        zs = ("<=%d chunks (⚠️ [ReadFile] caps at 6/fd — not a measurement), "
              "max off 0x%x (%.1f MB)" % (len(offs), max(offs), max(offs) / 1048576.0))
    else:
        zs = "OPENED but NEVER READ"
    if infl:
        zs += " | inflate %s calls, %.0f MB out" % (infl[-1][0], int(infl[-1][1]) / 1048576.0)
    print(f"   {m} min: {len(lines)} lines, {len(zone)} .ff opens | "
          f"{level}.ff fd={fd}: {zs}")
else:
    print(f"   {m} min: {len(lines)} lines, {len(zone)} .ff opens | "
          f"{level}.ff NOT OPENED YET")
for l in [x for x in lines if x.startswith("[QUEUE-WATCH]")][-2:]:
    print("       ", l[:130])
for pat in ("[STUB-CALL]", "=== CRASH", "[RUNAWAY]"):
    hits = [l for l in lines if l.startswith(pat)]
    if hits:
        print(f"        {pat} x{len(hits)}: {hits[0][:110]}")
PY
done

kill "$PID" 2>/dev/null; sleep 2
echo "--- summary ---"
python3 - "$LOG" <<'PY'
import re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()
for pat in ("[SPMAP]", "[FORCE-MAP]", "[DB-LOADXFILE]", "[QUEUE]",
            "[QUEUE-WATCH]", "[STUB-CALL]", "=== CRASH"):
    hits = [l for l in lines if l.startswith(pat)]
    if not hits:
        continue
    print(f"{pat} x{len(hits)}")
    for l in hits[:4]:
        print("   ", l[:150])
    if len(hits) > 6:
        print(f"    ... {len(hits)-6} more ...")
    for l in hits[-2:] if len(hits) > 4 else []:
        print("   ", l[:150])
PY

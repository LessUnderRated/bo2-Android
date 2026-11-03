#!/bin/bash
# Photograph a STALLED run from the outside: two host samples 30 s apart, then
# the in-process dumps. Nothing here needs a rebuild or an env var.
#
#   scripts/stall_probe.sh <pid> <outdir>
#
# WHY TWO SAMPLES. A single sample cannot tell a spin from a stall: an
# overlapping hot-offset set is a spin, a moving one is progress. And WHY FULL
# STACKS: the previous session read only the LEAF syscall of each thread
# (`__psynch_mutexwait`) and inferred which lock it was. The leaf names the
# syscall; only the frames above it name the mutex.
set -u
PID="${1:?usage: stall_probe.sh <pid> <outdir>}"
OUT="${2:?usage: stall_probe.sh <pid> <outdir>}"
mkdir -p "$OUT"
kill -0 "$PID" 2>/dev/null || { echo "no such pid $PID" >&2; exit 1; }

echo "[probe] sample A (10 s)..."
sample "$PID" 10 -mayDie -fullPaths -file "$OUT/sampleA.txt" >/dev/null 2>&1
echo "[probe] waiting 30 s..."
sleep 30
echo "[probe] sample B (10 s)..."
sample "$PID" 10 -mayDie -fullPaths -file "$OUT/sampleB.txt" >/dev/null 2>&1

# The in-process dumps go to the run's own stderr, not here; the caller reads
# them out of run.log. USR2 (GSC ring) works even when HUP's poll never fires.
echo "[probe] kill -HUP / -USR2..."
kill -HUP  "$PID" 2>/dev/null; sleep 3
kill -USR2 "$PID" 2>/dev/null; sleep 3

echo "--- every thread's LEAF syscall + the frame that called it ---"
python3 - "$OUT/sampleA.txt" "$OUT/sampleB.txt" <<'PY'
import re, sys

def threads(path):
    """Parse `sample`'s call-graph section into {thread-header: [frames]}."""
    out, cur = {}, None
    for ln in open(path, errors="replace"):
        if ln.startswith("    "+"Thread_") or re.match(r"^\s+\d+ Thread_", ln):
            cur = ln.strip(); out[cur] = []
        elif cur is not None and ln.strip().startswith("+"):
            out[cur].append(ln.rstrip())
        elif ln.startswith("Binary Images:"):
            break
    return out

for path in sys.argv[1:]:
    print(f"\n===== {path} =====")
    t = threads(path)
    if not t:
        # Fall back to the flat "Call graph" indentation form.
        print("  (no Thread_ headers parsed — dumping deepest lines)")
    for hdr, frames in t.items():
        print(f"  {hdr}")
        for f in frames[-6:]:
            print(f"    {f.strip()[:150]}")
PY
echo
echo "wrote $OUT/sampleA.txt $OUT/sampleB.txt"

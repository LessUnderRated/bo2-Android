#!/bin/bash
# replay_verify.sh — the stage-2 gate: does replaying ONE journal twice produce
# the same run twice?
#
#   scripts/replay_verify.sh <outdir> <journal> [runner] [extra BO2_* ...]
#
# runner defaults to scripts/menu_run.sh. The check is deliberately NOT "did it
# not crash": it compares the ordered sequence of engine messages, the crash
# chain, and the journal's own divergence report. A replay that ran to the end
# while serving the wrong values would pass a liveness check and fail this one.
set -u
OUT="${1:?usage: replay_verify.sh <outdir> <journal> [runner] [VAR=VAL ...]}"
JRNL="${2:?need a journal}"; shift 2
RUNNER="${1:-scripts/menu_run.sh}"; [ $# -gt 0 ] && shift
cd "$(dirname "$0")/.."
mkdir -p "$OUT"

[ -f "$JRNL" ] || { echo "no such journal: $JRNL" >&2; exit 2; }
python3 tools/jrnl.py "$JRNL"

for r in 1 2; do
    echo; echo "=== replay $r ==="
    "$RUNNER" "$OUT/r$r" BO2_REPLAY="$PWD/$JRNL" "$@" 2>&1 | tail -6
done

# GROUND TRUTH, and it does not depend on the journal's own bookkeeping: the
# ordered [IMPORT] stream of the RECORDING against each replay. Twice now the
# journal's self-report named a divergence that this comparison shows is not one.
RECLOG="$(dirname "$JRNL")/run.log"
if [ -f "$RECLOG" ]; then
    echo; echo "=== import-stream ground truth (recording vs replay) ==="
    for r in 1 2; do
        python3 tools/import_diff.py "$RECLOG" "$OUT/r$r/run.log" "rec-vs-r$r"
    done
    python3 tools/import_diff.py "$OUT/r1/run.log" "$OUT/r2/run.log" "r1-vs-r2"

    # UNCAPPED, whole-run: [IMP-HASH] is an order-sensitive rolling hash of the
    # import stream printed every 100,000 calls, so it covers the run the capped
    # [IMPORT] trace cannot. Two runs are the same run as far as these agree.
    echo "  --- [IMP-HASH], whole run (the [IMPORT] trace above stops at 100k) ---"
    python3 - "$RECLOG" "$OUT/r1/run.log" "$OUT/r2/run.log" <<'PH'
import sys, re
def hashes(p):
    return [l.strip() for l in open(p, errors='replace') if l.startswith('[IMP-HASH]')]
names = ["rec", "r1", "r2"]
H = [hashes(p) for p in sys.argv[1:4]]
for n, h in zip(names, H):
    print(f"    {n}: {len(h)} hash lines -> agreement provable through "
          f"import {max(0, len(h)-1)*100000}")
if not all(H):
    print("    (one run has none — it did not reach 100,000 imports)")
else:
    for i in range(min(len(x) for x in H)):
        vals = [x[i] for x in H]
        if len(set(vals)) != 1:
            print(f"    FIRST DIFFERING HASH LINE, index {i}:")
            for n, v in zip(names, vals): print(f"      {n}: {v}")
            break
    else:
        m = min(len(x) for x in H)
        print(f"    🎯 all {m} common hash lines IDENTICAL across rec, r1 and r2 "
              f"— exact agreement through import {(m-1)*100000}")
PH
fi

echo; echo "=== VERDICT ==="
python3 - "$OUT/r1/run.log" "$OUT/r2/run.log" <<'PY'
import sys, re, hashlib

def facts(path):
    txt = open(path, errors='replace').read().splitlines()
    msgs  = [re.sub(r'^\[ENGINE-MSG #\d+\] ', '', l) for l in txt if l.startswith('[ENGINE-MSG')]
    chain = [m for l in txt if l.strip().startswith('[chain] bp=')
               for m in re.findall(r'ret=([0-9a-f]+)', l)]
    div   = [l for l in txt if 'JRNL-DIVERGE' in l]
    val   = [l for l in txt if 'JRNL-VALUE'   in l]
    first = next((l for l in txt if 'JRNL-DIVERGE' in l), None)
    served = next((l for l in txt if l.startswith('[JRNL] records served')), '')
    return dict(lines=len(txt), msgs=msgs, chain=chain,
                ndiv=len(div), nval=len(val), first=first, served=served.strip())

a, b = facts(sys.argv[1]), facts(sys.argv[2])
def h(x): return hashlib.sha1("\n".join(x).encode()).hexdigest()[:12]

rows = [
    ("log lines",              a['lines'],       b['lines']),
    ("engine messages",        len(a['msgs']),   len(b['msgs'])),
    ("engine-message ORDER",   h(a['msgs']),     h(b['msgs'])),
    ("crash chain",            h(a['chain']),    h(b['chain'])),
    ("[JRNL-DIVERGE] count",   a['ndiv'],        b['ndiv']),
    ("[JRNL-VALUE] count",     a['nval'],        b['nval']),
]
w = max(len(r[0]) for r in rows)
for n, x, y in rows:
    print(f"  {n:<{w}}  {str(x):<20} {str(y):<20} {'ok' if x == y else 'DIFFER'}")
print(f"\n  r1 {a['served']}\n  r2 {b['served']}")
if a['first']: print(f"\n  r1 first divergence: {a['first'].strip()}")
if b['first']: print(f"  r2 first divergence: {b['first'].strip()}")

same = a['msgs'] == b['msgs'] and a['chain'] == b['chain']
clean = a['ndiv'] == 0 and b['ndiv'] == 0
print()
if same and clean:
    print("  🎯 PASS — two replays of one journal produced the same engine-message")
    print("     sequence and the same crash chain, with ZERO divergences.")
elif same:
    print("  PARTIAL — the two replays match each other, but each diverged from the")
    print("     journal. Replay is repeatable; it is not yet a replay of the RECORDING.")
else:
    print("  FAIL — the two replays differ from each other. Read the first row that")
    print("     says DIFFER; that is where to look.")
sys.exit(0 if (same and clean) else 1)
PY

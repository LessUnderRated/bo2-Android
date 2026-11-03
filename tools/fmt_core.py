"""fmt_core.py — the acceptance check for any change that touches per-thread state.

WHY. An interpose on the CRT's __getptd that returns the WRONG per-thread block
does not crash: it crosses errno/locale/CRT state between guest threads and
corrupts quietly. "It boots" proves nothing. The guest's own behaviour has to be
shown unchanged.

WHAT. Every distinct engine format string the run reached ([ENGINE-FMT]).

⚠️ CALIBRATED ON 185 RUNS, NOT 8. The first version took 8 runs, found a
104-format "core", and the very next unmodified run broke it with four formats
it had never seen (including `exceeded maximum number of parent %s script
variables`). An invariant fitted to 8 samples is not an invariant; it is a false
alarm generator. Across all 185 runs on disk that reached >=50 formats, only
**57** appear in EVERY one, and the known vocabulary is 132.

TWO SIGNALS, different strengths:
  * MISSING FROM THE CORE — one of the 57 that every run reaches did not appear.
    Strong: the guest stopped doing something it always does.
  * NEVER-SEEN FORMAT — a string absent from all 185 runs. Strong: the guest
    started doing something it has never done.
A format that is merely rarer than usual is NOT a signal; 75 of the 132 vary.

    python3 tools/fmt_core.py runs/<dir>/run.log [...]
"""
import json, re, sys, os

pat = re.compile(r"\[ENGINE-FMT #\d+\] (.*)$")
base = json.load(open(os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "docs", "engine_fmt_baseline.json")))
core, known = set(base["core"]), set(base["seen"])

rc = 0
for path in sys.argv[1:]:
    seen = set()
    for line in open(path, errors="replace"):
        m = pat.search(line)
        if m:
            seen.add(m.group(1))
    missing = core - seen
    novel = seen - known
    ok = not missing and not novel
    rc |= 0 if ok else 1
    print(f"{'OK  ' if ok else 'FAIL'} {path}  noyau {len(core & seen)}/{len(core)}"
          f"  vocabulaire connu {len(seen & known)}  JAMAIS VUS {len(novel)}")
    for f in sorted(missing):
        print(f"     MANQUANT du noyau : {f}")
    for f in sorted(novel):
        print(f"     JAMAIS VU en 185 runs : {f}")
sys.exit(rc)

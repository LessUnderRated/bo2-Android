#!/usr/bin/env python3
"""Resolve host PCs from a `sample` file to lifted functions, SIZE-AWARE.

WHY. `sample` attributes an address to the nearest PRECEDING symbol with no
regard for where that symbol ends. The lifted functions here are enormous and
adjacent, so a profile can name a neighbour: measured 2026-08-29, 619 samples
were attributed to sub_87e880, a function with two callers in the whole image,
both inside a drain that an interposed counter proved had already stopped.

This reads the boot binary's symbol table WITH SIZES and only claims a name when
the address really falls inside that symbol.

    tools/resolve_pc.py runs/prof-fast/s.txt

⚠️ NOT VALIDATED. It disagrees with `sample` on the same profile — same sample
weights (640, 619), different names — which at least confirms `sample` is wrong
there. But its own top entries come out at 3994, the whole thread's weight,
which means outer frames are being attributed to lifted symbols. Mach-O carries
no symbol sizes here (llvm-nm --print-size returns 0 for all 83,669 symbols), so
extents are derived from gaps, and a gap is an upper bound, not a size. Treat
both resolvers as hypotheses and prefer an interposed entry counter to either.
"""
import re, subprocess, sys, bisect

NM = "/opt/homebrew/opt/llvm@17/bin/llvm-nm"
BIN = "boot_cache/boot"

def table():
    # ⚠️ Mach-O symbols here carry NO size (all zero from llvm-nm --print-size),
    # so derive each one's extent from the gap to the next symbol. That is what
    # gives the bound `sample` lacks: an address 6 KB past a symbol start, in a
    # binary whose lifted functions are mostly a few hundred bytes, is a
    # NEIGHBOUR, not that function.
    out = subprocess.run([NM, "--defined-only", BIN],
                         capture_output=True, text=True).stdout
    raw = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 3:
            try: addr = int(p[0], 16)
            except ValueError: continue
            raw.append((addr, p[2]))
    raw.sort()
    syms = []
    for i, (a, nm) in enumerate(raw):
        nxt = raw[i + 1][0] if i + 1 < len(raw) else a + 0x1000
        syms.append((a, max(nxt - a, 1), nm))
    return syms, [a for a, _, _ in syms]

def main():
    syms, starts = table()
    counts = {}
    unresolved = 0
    for ln in open(sys.argv[1], errors="replace"):
        m = re.match(r'\s*[+!:|\s]*(\d+) .*?\[(0x[0-9a-f]+)', ln)
        if not m: continue
        n, pc = int(m.group(1)), int(m.group(2), 16)
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0:
            a, sz, nm = syms[i]
            if a <= pc < a + sz:
                counts[nm] = max(counts.get(nm, 0), n)
                continue
        unresolved += n
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1])[:15]:
        print("  %6d  %s" % (v, k))
    print("  (%d sample-weights fell outside every sized symbol)" % unresolved)

main()

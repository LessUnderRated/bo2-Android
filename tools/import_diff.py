#!/usr/bin/env python3
"""Compare the ordered [IMPORT] streams of two runs. Ground truth for replay.

    tools/import_diff.py <logA> <logB> [label]

Reports the first ordinal at which the two runs asked for a different import, on
a different thread, or from a different call site. This does NOT depend on the
journal's own bookkeeping — twice the journal's self-report named a divergence
that this comparison showed was not one.

⚠️ The [IMPORT] trace is capped at 100,000 lines. Both runs are truncated at the
SAME ordinal, so comparing within it is like-for-like; but "identical to the end
of the window" means identical to 100k, NOT identical for the whole run. The
output says which of the two it is.
"""
import re, sys

PAT = re.compile(r'^\[IMPORT\] (\S+) \(call #(\d+), ESP=0x[0-9a-f]+, '
                 r'ret=(0x[0-9a-f]+), EBP=0x[0-9a-f]+\) tid=(\d+)')

def load(path):
    d = {}
    for line in open(path, errors='replace'):
        if line.startswith('[IMPORT]'):
            m = PAT.match(line)
            if m:
                d[int(m.group(2))] = (m.group(1), m.group(3), int(m.group(4)))
    return d

def main(a, b, label):
    A, B = load(a), load(b)
    if not A or not B:
        print(f"  {label}: no [IMPORT] trace in one of the logs "
              f"(A={len(A)}, B={len(B)}) — nothing to compare")
        return
    common = min(max(A), max(B))
    first = None
    for i in range(common + 1):
        if i in A and i in B and A[i] != B[i]:
            first = i
            break
    if first is None:
        print(f"  {label}: IDENTICAL for all {min(len(A), len(B))} traced imports "
              f"(the trace is capped at 100,000 — this is the whole window, "
              f"not the whole run)")
        return
    print(f"  {label}: identical for {first} imports, then")
    for tag, S in (("rec/A", A), ("rep/B", B)):
        n, r, t = S[first]
        print(f"      {tag}  #{first}  {n:<36} ret={r} tid={t}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "diff")

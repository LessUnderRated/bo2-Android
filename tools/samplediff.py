#!/usr/bin/env python3
"""Diff two macOS `sample` captures of the SAME process, on the guest thread.

    tools/samplediff.py /tmp/two_early.txt /tmp/two_late.txt

Every guest coroutine runs on ONE host thread (the cooperative scheduler), so the
only meaningful comparison is that thread's LEAF distribution. Percentages, not
raw counts: the two captures may differ in length.

Why leaves and not the call tree: `sample` splits one function across a dozen
offset lines and the tree double-counts nested frames, so counting distinct lines
once scored a 100%-busy guest function below gfx_pump_once (see
bo2-spin-inside-one-lifted-block). Leaf samples are the only additive quantity.
"""
import argparse, re, sys


def leaves(path):
    lines = open(path, encoding='latin1').readlines()
    end = next((i for i, l in enumerate(lines) if l.startswith('Sort by top of stack')), len(lines))
    lines = lines[:end]

    def dep(l):
        m = re.match(r'^([\s+|:!]*)\d+ ', l)
        return len(m.group(1)) if m else None

    def sym(l):
        m = re.match(r'^[\s+|:!]*\d+ (.*?)(  \(in |  \[0x|$)', l.rstrip('\n'))
        return m.group(1).strip() if m else ''

    def cnt(l):
        m = re.match(r'^[\s+|:!]*(\d+) ', l)
        return int(m.group(1)) if m else 0

    # the host thread carrying guest code = most sub_* samples
    best, bestn, idx = None, -1, None
    for i, l in enumerate(lines):
        m = re.match(r'^\s*(\d+) (Thread_\d+.*?)\s*$', l)
        if not m or 'Thread_' not in m.group(2):
            continue
        b, n, j = dep(l), 0, i + 1
        while j < len(lines):
            d = dep(lines[j])
            if d is None:
                j += 1; continue
            if d <= b:
                break
            if re.match(r'_?sub_[0-9a-f]+', sym(lines[j])):
                n += cnt(lines[j])
            j += 1
        if n > bestn:
            bestn, best, idx = n, m.group(2), i
    if idx is None:
        return None, {}
    base = dep(lines[idx])
    rows = []
    for l in lines[idx + 1:]:
        d = dep(l)
        if d is None:
            continue
        if d <= base:
            break
        rows.append((d, sym(l), cnt(l)))
    out, tot = {}, 0
    for i, (d, s, c) in enumerate(rows):
        if (i + 1 < len(rows)) and rows[i + 1][0] > d:
            continue                      # not a leaf
        out[s] = out.get(s, 0) + c
        tot += c
    return (best, tot), {k: 100.0 * v / tot for k, v in out.items()} if tot else {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('early'); ap.add_argument('late')
    ap.add_argument('-n', type=int, default=18)
    # Two captures of two DIFFERENT runs (e.g. afghanistan vs angola) never
    # share a thread id: the guest thread is still the one with the most sub_*.
    ap.add_argument('--cross-run', action='store_true',
                    help='the two captures come from two processes: allow different thread ids')
    ap.add_argument('--ms-early', type=float,
                    help='ms per tick (or frame) during the EARLY window')
    ap.add_argument('--ms-late', type=float,
                    help='ms per tick (or frame) during the LATE window')
    a = ap.parse_args()
    (te, ne), e = leaves(a.early)
    (tl, nl), l = leaves(a.late)
    print("early: %s  %d leaf samples" % (te.split()[0], ne))
    print("late : %s  %d leaf samples" % (tl.split()[0], nl))
    # ⚠️ A GHOST: if the two windows picked DIFFERENT host threads, every row is
    # a comparison between two unrelated things.
    if te.split()[0] != tl.split()[0] and not a.cross_run:
        print("\n\u26a0\ufe0f  DIFFERENT THREADS (%s vs %s) — this diff is meaningless."
              % (te.split()[0], tl.split()[0]))
        return

    keys = set(e) | set(l)
    rows = [(k, e.get(k, 0.0), l.get(k, 0.0)) for k in keys]
    rows = [r for r in rows if max(r[1], r[2]) >= 0.4]

    # ⚠️ `sample` counts by TIME, so a fixed-length window covers FEWER ticks
    # late (the tick got slower). A function whose cost per tick never changed
    # therefore loses share, and one whose per-tick cost really doubled can look
    # flat. Share answers "what dominates"; only ms-per-tick answers "did this
    # get more expensive". Give --ms-early/--ms-late to get the second.
    per_tick = a.ms_early and a.ms_late
    if per_tick:
        print("\n   normalised to MS PER TICK (early %.0f ms, late %.0f ms — x%.2f)"
              % (a.ms_early, a.ms_late, a.ms_late / a.ms_early))
        print("%-40s %9s %9s %8s" % ("guest-thread leaf", "early ms", "late ms", "ratio"))
        conv = lambda pct, ms: pct / 100.0 * ms
        scored = [(k, conv(ee, a.ms_early), conv(ll, a.ms_late)) for k, ee, ll in rows]
        for k, ee, ll in sorted(scored, key=lambda r: -(r[2] - r[1]))[:a.n]:
            r = (ll / ee) if ee > 1e-9 else float('inf')
            print("  %-38s %8.2f %9.2f %7s" % (k[:38], ee, ll,
                  "%.2f" % r if r != float('inf') else "new"))
        print("  ...")
        for k, ee, ll in sorted(scored, key=lambda r: (r[2] - r[1]))[:6]:
            r = (ll / ee) if ee > 1e-9 else float('inf')
            print("  %-38s %8.2f %9.2f %7s" % (k[:38], ee, ll,
                  "%.2f" % r if r != float('inf') else "new"))
    else:
        print("\n   \u26a0\ufe0f  SHARE ONLY — pass --ms-early/--ms-late for cost per tick.")
        print("%-46s %8s %8s %9s" % ("guest-thread leaf", "early%", "late%", "delta pp"))
        for k, ee, ll in sorted(rows, key=lambda r: -(r[2] - r[1]))[:a.n]:
            print("  %-44s %7.1f%% %7.1f%%  %+8.1f" % (k[:44], ee, ll, ll - ee))


if __name__ == '__main__':
    main()

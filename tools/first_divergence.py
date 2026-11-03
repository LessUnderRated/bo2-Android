#!/usr/bin/env python3
"""Find the FIRST line where two run logs stop agreeing.

A pass/fail pair answers "what changed?" far better than either log alone, but
the answer is one line buried in ~10^5, and the interesting part is always the
LAST line they share -- everything after it in the healthy run is what the sick
run failed to do. Doing this by hand on 2026-09-12 is what located the angola
fast-barrier divergence (both runs identical through DBLOADX #9, then one went on
to load LUI and the other raised Com_Error).

Volatile fields are masked before comparing, because two runs never agree on
them and they are not the signal:
  * every hex literal      -> H     (addresses, handles, ids)
  * every decimal run      -> N     (counters, sizes, tids, timings)
Lines that differ only in masked fields are therefore EQUAL here, which is the
point: we want the first structural divergence, not the first byte difference.

Usage:
    tools/first_divergence.py runs/aa1/run.log runs/bar_fast1/run.log.gz
    tools/first_divergence.py A B --context 12 --skip '^\\[DBWAIT\\]'

Handles .gz transparently. Streams both files; never loads either into memory.
"""
import argparse
import gzip
import io
import re
import sys

HEX = re.compile(rb'0[xX][0-9a-fA-F]+')
DEC = re.compile(rb'\d+')


def _open(path):
    if path.endswith('.gz'):
        return io.TextIOWrapper(gzip.open(path, 'rb'), errors='replace')
    return open(path, errors='replace')


def mask(line):
    b = line.encode('utf-8', 'replace')
    return DEC.sub(b'N', HEX.sub(b'H', b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--context', type=int, default=8,
                    help='lines to show from each side after the divergence')
    ap.add_argument('--skip', default=None,
                    help='regex of lines to ignore on both sides (noise like [DBWAIT])')
    args = ap.parse_args()
    skip = re.compile(args.skip) if args.skip else None

    def lines(path):
        # (line number in the real file, text) -- numbering survives --skip so the
        # reported line is the one you can actually go and look at.
        with _open(path) as fh:
            for n, raw in enumerate(fh, 1):
                t = raw.rstrip('\n')
                if skip and skip.search(t):
                    continue
                yield n, t

    ga, gb = lines(args.a), lines(args.b)
    last_common = None
    na = nb = 0
    while True:
        ra, rb = next(ga, None), next(gb, None)
        if ra is None or rb is None:
            print(f'no divergence: {"A" if ra is None else "B"} simply ends first '
                  f'(A line {na}, B line {nb})')
            return 0
        na, ta = ra
        nb, tb = rb
        if mask(ta) != mask(tb):
            print(f'last line in common : A:{last_common[0] if last_common else 0} '
                  f'B:{last_common[1] if last_common else 0}')
            if last_common:
                print(f'  {last_common[2]}')
            print(f'\nFIRST DIVERGENCE    : A:{na}  B:{nb}')
            print(f'  A< {ta}')
            print(f'  B> {tb}')
            print(f'\n--- next {args.context} in A ({args.a}) ---')
            for _ in range(args.context):
                r = next(ga, None)
                if not r:
                    break
                print(f'  A< {r[1]}')
            print(f'\n--- next {args.context} in B ({args.b}) ---')
            for _ in range(args.context):
                r = next(gb, None)
                if not r:
                    break
                print(f'  B> {r[1]}')
            return 1
        last_common = (na, nb, ta)


if __name__ == '__main__':
    sys.exit(main())

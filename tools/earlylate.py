#!/usr/bin/env python3
"""Compare every [CADENCE] counter EARLY vs LATE inside one run, per FRAME.

    tools/earlylate.py runs/o2_1/run.log
    tools/earlylate.py runs/o2_*/run.log runs/afg1/run.log --filter

Counter deltas are normalised by frames or server ticks. Time is the measured
CADENCE elapsed_ms; untimed legacy logs require --period explicitly. Windows
include zero-progress samples after server startup, including terminal stalls.
"""
import argparse, re, statistics, sys
from cadence import read_samples


def samples(path, period=None, after=None):
    out = []
    for counters, elapsed, measured in read_samples(path, period, after):
        counters['_elapsed_ms'] = elapsed
        counters['_timed'] = measured
        out.append(counters)
    return out


def per_frame(win, unit='guestFrames'):
    """{counter: delta per unit} summed over a window of samples.

    unit='guestFrames' answers "work per rendered frame"; unit='svWorker'
    answers "work per SERVER TICK", which is the physical unit for server-side
    work and the one that matters once the engine starts running more than one
    tick per frame (measured: 0.49 -> 1.13 ticks/frame in angola).
    """
    frames = sum(s.get(unit, (0, 0))[1] for s in win)
    if frames == 0:
        return None, 0
    keys = {k for s in win for k in s if not k.startswith('_')}
    return ({k: sum(s.get(k, (0, 0))[1] for s in win) / float(frames) for k in keys},
            frames)


def analyse(path, nwin=2, unit='guestFrames', period=None, after=None):
    if nwin < 1:
        raise ValueError('--window must be positive')
    ss = samples(path, period, after)
    # Retain every interval after server startup, including no-progress windows.
    start = 0 if after is not None and ss else next((i for i, s in enumerate(ss)
                  if s.get('svWorker', (0, 0))[1] > 0), None)
    if start is None:
        return None
    lvl = ss[start:]
    if len(lvl) < 2 * nwin:
        return None
    early, ef = per_frame(lvl[:nwin], unit)
    late, lf = per_frame(lvl[-nwin:], unit)
    if not early:
        return None
    ms_e = sum(s['_elapsed_ms'] for s in lvl[:nwin]) / ef
    ms_l = sum(s['_elapsed_ms'] for s in lvl[-nwin:]) / lf if lf else float('inf')
    late = late or {}
    rows = []
    for k in sorted(set(early) & set(late)):
        e, l = early[k], late[k]
        if e < 1e-9 and l < 1e-9:
            continue
        ratio = (l / e) if e > 1e-9 else float('inf')
        rows.append((k, e, l, ratio))
    return {'run': path, 'ms_early': ms_e, 'ms_late': ms_l,
            'tick_ratio': ms_l / ms_e if ms_e else 0, 'rows': rows,
            'frames_early': ef, 'frames_late': lf, 'level_samples': len(lvl),
            'timing': 'measured' if all(s['_timed'] for s in lvl) else 'legacy estimate'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--period', type=float, default=None,
                    help='explicit interval estimate for legacy untimed logs (seconds)')
    ap.add_argument('--after', help='select full intervals after the first regex match; discard the crossing interval')
    ap.add_argument('--window', type=int, default=2, help='samples per end')
    ap.add_argument('--per-tick', action='store_true',
                    help='normalise by server TICK (svWorker) instead of frame')
    ap.add_argument('--filter', action='store_true',
                    help='keep only counters whose growth could explain the tick')
    a = ap.parse_args()
    print('SELECTION: ' + ('after ' + a.after if a.after else 'first worker entry; includes loading, not a gameplay marker'))
    for p in a.logs:
        try:
            r = analyse(p, a.window, 'svWorker' if a.per_tick else 'guestFrames', a.period, a.after)
        except ValueError as e:
            ap.error(str(e))
        if not r:
            print("=== %s: too few level samples\n" % p); continue
        if r['timing'] != 'measured':
            print('TIMING: legacy estimate using --period, not measured elapsed time.')
        tr = r['tick_ratio']
        print("=== %s   %d level samples   interval %.0f -> %.0f ms  (x%.1f)"
              % (p, r['level_samples'], r['ms_early'], r['ms_late'], tr))
        lin, quad = tr, tr ** 0.5
        rows = r['rows']
        if a.filter:
            rows = [x for x in rows if 0.6 * quad <= x[3] <= 1.8 * lin]
            print("    (arithmetic filter: keeping growth in [%.1fx, %.1fx] — "
                  "quadratic %.1fx .. linear %.1fx)" % (0.6 * quad, 1.8 * lin, quad, lin))
        print("    %-22s %12s %12s %8s" % ("counter (per %s)" % ("tick" if a.per_tick else "frame"), "early", "late", "ratio"))
        for k, e, l, ratio in sorted(rows, key=lambda x: -x[3]):
            flag = ""
            if quad * 0.6 <= ratio <= lin * 1.8:
                flag = "  <-- could explain a x%.1f tick" % tr
            print("    %-22s %12.2f %12.2f %8.2f%s" % (k, e, l, ratio, flag))
        print()


if __name__ == '__main__':
    main()

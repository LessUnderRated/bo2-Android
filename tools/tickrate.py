#!/usr/bin/env python3
"""ms per server tick — the CONTINUOUS outcome variable for angola A/Bs.

    tools/tickrate.py runs/*/run.log
    tools/tickrate.py --json runs/spin1/run.log

Why this exists (probe rule (h)): angola's "freeze" is a DECELERATION, so
"still advancing at 300 s" is a THRESHOLD on a continuous quantity, not a fact.
A flag that speeds things up 20% crosses that threshold in some runs and not
others -- which is the whole of 2026-09-07's non-replication (6/7 then 0/6,
4/7, 3/7, 2/7, 1/7). An A/B must report an EFFECT SIZE, not a coin flip.

CADENCE counters measure progress over host-clock intervals, not CPU time inside
SV_Frame. New logs record elapsed_ms. Legacy logs need --period explicitly;
20 seconds was previously assumed incorrectly. Zero-tick samples after the first
server tick remain part of elapsed-time accounting.
"""
import argparse, gzip, json, math, re, statistics, sys
from cadence import read_samples


def parse(path, period=None, after=None):
    lvl, pre = [], []
    started = after is not None
    for counters, elapsed, measured in read_samples(path, period, after):
        sw_d = counters.get('svWorker', (0, 0))[1]
        fr_d = counters.get('guestFrames', (0, 0))[1]
        started = started or sw_d > 0
        (lvl if started else pre).append((sw_d, fr_d, elapsed, measured))
    return lvl, pre


def is_complete(path):
    """A run that is still going has fewer samples than it will have, and its
    median is a different number from the one it ends with. Reading one cost a
    published "afghanistan is FLAT, 1.03x" that the finished run contradicted
    (3 level samples vs 8). Two independent checks, both cheap:
      * the teardown report ([ERR-CAT]) is only written when the run ends;
      * a log touched in the last 30 s is probably still being appended to.
    """
    import os, time
    try:
        recent = (time.time() - os.path.getmtime(path)) < 30.0
    except OSError:
        recent = False
    tail = ""
    try:
        if str(path).endswith('.gz'):
            with gzip.open(path, 'rb') as f:
                tail = f.read()[-300000:].decode('latin1', 'replace')
        else:
            with open(path, 'rb') as f:
                f.seek(max(0, os.path.getsize(path) - 300000))
                tail = f.read().decode('latin1', 'replace')
    except OSError:
        pass
    return ('[ERR-CAT]' in tail) and not recent


def summarise(path, period=None, after=None):
    lvl, pre = parse(path, period, after)
    if not lvl:
        return {'run': path, 'level_samples': 0, 'selection': 'after regex: ' + after if after is not None else 'first worker entry; includes loading', 'note': 'no full intervals after marker' if after is not None else 'no worker entries'}
    ticks = [row[0] for row in lvl]
    ms = [elapsed / sw if sw else float('inf') for sw, _, elapsed, _ in lvl]
    frames = [row[1] for row in lvl]
    finite = [v for v in ms if math.isfinite(v)]
    median = statistics.median(ms)
    return {
        'run': path,
        'complete': is_complete(path),
        'selection': 'after regex: ' + after if after is not None else 'first worker entry; includes loading',
        'timing': 'measured' if all(row[3] for row in lvl) else 'legacy estimate',
        'level_samples': len(lvl),
        'zero_tick_samples': ticks.count(0),
        'elapsed_ms': round(sum(row[2] for row in lvl), 3),
        'ticks_total': sum(ticks),
        'ms_per_tick_mean': round(sum(row[2] for row in lvl) / sum(ticks), 1) if sum(ticks) else None,
        'ms_per_tick_floor': round(min(finite), 1) if finite else None,
        'ms_per_tick_median': round(median, 1) if math.isfinite(median) else None,
        'ms_per_tick_sd': round(statistics.stdev(finite), 1) if len(finite) > 1 else 0.0,
        'ms_per_tick_min': round(min(finite), 1) if finite else None,
        'ms_per_tick_max': round(max(ms), 1) if all(ticks) else None,
        'frames_per_sample_mean': round(statistics.mean(frames), 1),
        'pre_level_frames_mean': round(statistics.mean([row[1] for row in pre]), 1) if pre else None,
        'reached_zero': ticks[-1] == 0 or frames[-1] == 0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--period', type=float, default=None, help='explicit interval estimate for legacy untimed logs (seconds)')
    ap.add_argument('--after', help='select full intervals after the first regex match; discard the crossing interval')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    try:
        rows = [summarise(p, a.period, a.after) for p in a.logs]
    except ValueError as e:
        ap.error(str(e))
    if a.json:
        print(json.dumps(rows, indent=1, allow_nan=False)); return
    if any(r.get('timing') == 'legacy estimate' for r in rows):
        print('TIMING: legacy estimate using --period; not measured elapsed time.')
    unfinished = [r['run'] for r in rows if not r.get('complete')]
    if unfinished:
        print("\n\u26a0\ufe0f  STILL RUNNING (or truncated) \u2014 these numbers WILL change:")
        for u in unfinished:
            print("      %s" % u)
        print("   A median read mid-run is a different number from the final one.\n")
    print('SELECTION: ' + ('after ' + a.after if a.after else 'first worker entry; includes loading, not a gameplay marker'))
    print("%-22s %6s %8s %9s %8s %8s %8s" % (
        "run", "ticks", "FLOOR", "median", "mean", "sd", "pre-lvl"))
    for r in rows:
        if not r.get('level_samples'):
            print("%-22s  -- %s" % (r['run'][:22], r.get('note'))); continue
        print("%-22s %6d %8.1f %9.1f %8.1f %8.1f %8s" % (
            r['run'][:22], r['ticks_total'], (r['ms_per_tick_floor'] if r['ms_per_tick_floor'] is not None else float('inf')), (r['ms_per_tick_median'] if r['ms_per_tick_median'] is not None else float('inf')),
            (r['ms_per_tick_mean'] if r['ms_per_tick_mean'] is not None else float('inf')), r['ms_per_tick_sd'], r['pre_level_frames_mean']))
    good = [r for r in rows if r.get('level_samples')]
    if len(good) > 1:
        for label, key in (("FLOOR ", 'ms_per_tick_floor'),
                           ("median", 'ms_per_tick_median'),
                           ("mean  ", 'ms_per_tick_mean')):
            m = [r[key] for r in good if r[key] is not None]
            if len(m) < 2:
                continue
            mu, sd = statistics.mean(m), statistics.stdev(m)
            print("BASELINE %s over %d runs: %8.1f +/- %6.1f ms   (rel. spread %4.0f%%)"
                  % (label, len(m), mu, sd, 100.0 * sd / mu))
        print("\nUse the FLOOR: it is the least contaminated by the deceleration and the")
        print("most stable across runs, so it needs the smallest effect size to be read.")
        print("An A/B must beat its spread, and n=1 cannot.")


if __name__ == '__main__':
    main()

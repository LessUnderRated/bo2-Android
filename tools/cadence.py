"""Read timed CADENCE samples; legacy logs require an explicit period estimate."""
import gzip
import math
import re

COUNTER = re.compile(r'([A-Za-z][A-Za-z0-9_ ]*?)=(\d+)\(\+(\d+)\)')
ELAPSED = re.compile(r'\belapsed_ms=(\S+)')


def read_samples(path, period=None, after=None):
    if period is not None and (not math.isfinite(period) or period <= 0):
        raise ValueError('--period must be a positive finite number of seconds')
    try:
        marker = re.compile(after) if after is not None else None
    except re.error as e:
        raise ValueError('invalid --after regex: %s' % e)
    selected = marker is None
    discard_boundary = False
    opener = gzip.open if str(path).endswith('.gz') else open
    samples = []
    with opener(path, 'rt', encoding='latin1') as stream:
        for line in stream:
            if not selected:
                if marker.search(line):
                    selected = True
                    discard_boundary = True
                continue
            if not line.startswith('[CADENCE]'):
                continue
            counters = {m[1].strip(): (int(m[2]), int(m[3]))
                        for m in COUNTER.finditer(line)}
            if not counters:
                continue
            if discard_boundary:
                # This interval began before the marker; its deltas cannot be split.
                discard_boundary = False
                continue
            match = ELAPSED.search(line)
            if match:
                try:
                    elapsed = float(match[1])
                except ValueError:
                    raise ValueError('%s: invalid CADENCE elapsed time' % path)
                if not math.isfinite(elapsed) or elapsed < 0:
                    raise ValueError('%s: invalid CADENCE elapsed time' % path)
                if elapsed == 0:
                    continue  # First report has no previous timestamp.
            elif period is None:
                raise ValueError('%s: CADENCE has no elapsed_ms; supply --period '
                                 'SECONDS explicitly for a legacy estimate '
                                 '(HUD nominal period is about 5s, not 20s)' % path)
            else:
                elapsed = period * 1000.0
            samples.append((counters, elapsed, match is not None))
    if not selected:
        raise ValueError('%s: --after marker not found: %s' % (path, after))
    return samples

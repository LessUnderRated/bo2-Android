#!/usr/bin/env python3
"""Timing regression tests: physical units, variable intervals, and stalls."""
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from cadence import read_samples
from earlylate import analyse
from tickrate import summarise


def line(ticks, frames, elapsed=None):
    return ('[CADENCE] svWorker=100(+%d) gscRun=100(+2) '
            'guestFrames=100(+%d)%s\n' %
            (ticks, frames, '' if elapsed is None else ' elapsed_ms=%s' % elapsed))


class TimingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'run.log'

    def tearDown(self):
        self.temp.cleanup()

    def log(self, text):
        self.path.write_text(text)
        return str(self.path)

    def test_legacy_requires_explicit_period(self):
        path = self.log(line(10, 10))
        with self.assertRaisesRegex(ValueError, 'no elapsed_ms'):
            summarise(path)
        self.assertEqual(summarise(path, 5)['ms_per_tick_floor'], 500)

    def test_real_time_overrides_estimate(self):
        r = summarise(self.log(line(10, 10, 6000)), 20)
        self.assertEqual(r['ms_per_tick_floor'], 600)
        self.assertEqual(r['timing'], 'measured')

    def test_irregular_intervals_equal_rate(self):
        p = self.log(line(10, 20, 5000) + line(20, 40, 10000))
        r = analyse(p, 1, 'svWorker')
        self.assertEqual((r['ms_early'], r['ms_late'], r['tick_ratio']), (500, 500, 1))

    def test_zero_progress_is_elapsed_time(self):
        p = self.log(line(0, 10, 5000) + line(10, 10, 5000) + line(0, 0, 10000))
        r = summarise(p)
        self.assertEqual(r['level_samples'], 2)
        self.assertEqual(r['zero_tick_samples'], 1)
        self.assertEqual(r['ms_per_tick_mean'], 1500)
        self.assertTrue(r['reached_zero'])

    def test_terminal_stall_not_filtered_from_late_window(self):
        p = self.log(line(10, 10, 5000) + line(0, 0, 5000))
        self.assertEqual(analyse(p, 1)['ms_late'], float('inf'))

    def test_first_untimed_baseline_is_excluded(self):
        r = summarise(self.log(line(9999, 9999, 0) + line(10, 10, 5000)))
        self.assertEqual(r['ticks_total'], 10)

    def test_phase_discards_crossing_interval(self):
        p = self.log(line(100, 1000, 5000) + '[SPAWN] ready\n' +
                     line(80, 800, 5000) + line(10, 20, 6000))
        r = summarise(p, after=r'^\[SPAWN\]')
        self.assertEqual((r['ticks_total'], r['elapsed_ms']), (10, 6000))
        self.assertEqual(r['ms_per_tick_mean'], 600)

    def test_selected_phase_can_have_no_progress(self):
        p = self.log('[SPAWN] ready\n' + line(80, 800, 5000) + line(0, 0, 6000))
        r = summarise(p, after=r'^\[SPAWN\]')
        self.assertEqual(r['zero_tick_samples'], 1)
        self.assertEqual(r['elapsed_ms'], 6000)
        self.assertIsNone(r['ms_per_tick_mean'])
        self.assertIsNone(r['ms_per_tick_floor'])

    def test_missing_marker_is_not_a_negative_measurement(self):
        with self.assertRaisesRegex(ValueError, 'marker not found'):
            summarise(self.log(line(10, 20, 6000)), after='SPAWN')

    def test_repeated_marker_does_not_reset_phase(self):
        p = self.log('SPAWN\n' + line(80, 800, 5000) + line(10, 20, 6000) +
                     'SPAWN\n' + line(20, 40, 12000))
        r = analyse(p, 1, after='SPAWN')
        self.assertEqual((r['ms_early'], r['ms_late']), (300, 300))

    def test_gzip(self):
        p = self.path.with_suffix('.log.gz')
        with gzip.open(p, 'wt') as f:
            f.write(line(10, 10, 5000))
        self.assertEqual(summarise(str(p))['ms_per_tick_floor'], 500)

    def test_invalid_period(self):
        p = self.log(line(10, 10))
        for period in (0, -1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                read_samples(p, period)

    def test_invalid_timestamp_cannot_use_legacy_fallback(self):
        for elapsed in ('nan', 'inf', '-5', 'broken'):
            with self.assertRaisesRegex(ValueError, 'invalid CADENCE'):
                read_samples(self.log(line(10, 10, elapsed)), 5)

    def test_json_output_remains_valid_during_stall(self):
        p = self.log(line(10, 10, 5000) + line(0, 0, 5000) * 3)
        tool = Path(__file__).with_name('tickrate.py')
        r = subprocess.run([sys.executable, str(tool), '--json', p],
                           capture_output=True, text=True, check=True)
        data = json.loads(r.stdout)
        self.assertIsNone(data[0]['ms_per_tick_median'])
        self.assertNotIn('Infinity', r.stdout)


if __name__ == '__main__':
    unittest.main()

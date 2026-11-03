#!/usr/bin/env python3
"""Regression checks for the level-rendering measurement window."""
import pathlib
import tempfile
import unittest
from gameplay_speed import analyse


class WindowTests(unittest.TestCase):
    def measure(self, text):
        with tempfile.TemporaryDirectory() as d:
            pathlib.Path(d, 'run.log').write_text(text)
            return analyse(d)

    def test_crossing_window_excluded_and_stall_retained(self):
        row = self.measure('''[BINARY] sha256=1234567890abcdef
[CADENCE] guestFrames=100(+100) elapsed_ms=5000
[DRAWS] this frame 501 records
[CADENCE] guestFrames=190(+90) elapsed_ms=5000
[CADENCE] guestFrames=200(+10) elapsed_ms=5000
[DRAWS] this frame 600 records
[CADENCE] guestFrames=200(+0) elapsed_ms=5000
''')
        self.assertEqual(row['fps'], [2, 0])
        self.assertEqual((row['frames'], row['secs']), (10, 10))

    def test_crossing_only_is_not_measured(self):
        row = self.measure('[DRAWS] this frame 500 records\n'
                           '[CADENCE] guestFrames=90(+90) elapsed_ms=5000\n')
        self.assertEqual(row['nwin'], 0)

    def test_spawn_alone_is_not_rendering(self):
        row = self.measure('[SPAWN] info_player_start #1\n'
                           '[CADENCE] guestFrames=90(+90) elapsed_ms=5000\n')
        self.assertIsNone(row['marker'])
        self.assertEqual(row['nwin'], 0)


if __name__ == '__main__':
    unittest.main()

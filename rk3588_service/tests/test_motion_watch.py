# -*- coding: ascii -*-
"""Host unit tests for motion_watch.MotionWatch (injected clock)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import motion_watch  # noqa: E402

try:
    import numpy as np
except Exception:                       # pragma: no cover
    np = None


class FakeClock(object):
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


@unittest.skipIf(np is None, "no numpy on host")
class TestMotionWatch(unittest.TestCase):
    def _sig(self, value):
        a = np.zeros((48, 64), np.uint8)
        a[:] = value
        return a

    def test_starts_awake(self):
        w = motion_watch.MotionWatch(clock=FakeClock())
        self.assertTrue(w.motion_recent())

    def test_identical_frames_go_quiet(self):
        c = FakeClock()
        w = motion_watch.MotionWatch(threshold=2.0, hold_s=9.0, clock=c)
        sig = self._sig(128)
        w.feed(sig)
        w.feed(sig)
        self.assertTrue(w.motion_recent())
        c.now += 9.1
        self.assertFalse(w.motion_recent())

    def test_motion_refreshes_window(self):
        c = FakeClock()
        w = motion_watch.MotionWatch(threshold=2.0, hold_s=9.0, clock=c)
        a, b = self._sig(0), self._sig(80)   # MAD 80 >> threshold
        w.feed(a)
        w.feed(b)
        c.now += 5.0
        self.assertTrue(w.motion_recent())
        w.feed(b)
        w.feed(self._sig(80))              # still identical, no refresh
        c.now += 5.1                       # 10.1s since the only motion
        self.assertFalse(w.motion_recent())

    def test_small_noise_below_threshold(self):
        c = FakeClock()
        w = motion_watch.MotionWatch(threshold=2.0, hold_s=1.0, clock=c)
        a = self._sig(100)
        b = self._sig(101)                 # MAD 1.0 - jpeg noise level
        w.feed(a)
        w.feed(b)
        c.now += 1.1
        self.assertFalse(w.motion_recent())

    def test_note_motion_pins_window(self):
        c = FakeClock()
        w = motion_watch.MotionWatch(hold_s=9.0, clock=c)
        c.now += 20.0
        self.assertFalse(w.motion_recent())
        w.note_motion()
        self.assertTrue(w.motion_recent())


if __name__ == "__main__":
    unittest.main()

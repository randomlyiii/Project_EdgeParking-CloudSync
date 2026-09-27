# -*- coding: ascii -*-
"""Motion watch - decides whether the camera scene is alive or static.

CameraThread feeds down-sampled grayscale frames at capture rate; the watch
keeps the timestamp of the last frame pair whose mean absolute difference
crossed `threshold`. RecogThread sleeps while no motion happened within
`hold_s`, so a static scene (empty parking spot) costs ~zero recognition
CPU. numpy is imported lazily to keep the module host-testable.

One hold window replaces a whole state machine: it is simultaneously the
fall-asleep hysteresis, the wake trigger (motion refreshes the stamp
immediately, no waiting for the next recognition period) and the cold-start
rule (last_motion starts at "now", so the first hold_s seconds always run -
a car already parked when the system boots gets recognized).
"""

import time


class MotionWatch(object):
    """feed(sig) with 2D grayscale arrays; motion_recent() queries state."""

    def __init__(self, threshold=2.0, hold_s=9.0, clock=None):
        self.threshold = float(threshold)
        self.hold_s = float(hold_s)
        self._clock = clock or time.monotonic
        self._prev = None
        self._last_motion = self._clock()

    def feed(self, sig):
        """sig: small grayscale ndarray (e.g. 64x48)."""
        if self._prev is not None:
            import numpy as np
            diff = float(np.abs(self._prev.astype(np.int16) - sig).mean())
            if diff >= self.threshold:
                self._last_motion = self._clock()
        self._prev = sig

    def motion_recent(self, now=None):
        now = self._clock() if now is None else now
        return (now - self._last_motion) < self.hold_s

    def note_motion(self):
        """External bump (e.g. a fresh plate candidate) - not used by the
        gate itself; kept for callers that want to pin the window open."""
        self._last_motion = self._clock()

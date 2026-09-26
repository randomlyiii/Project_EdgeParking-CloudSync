# -*- coding: ascii -*-
"""Consecutive-frame plate voting + emit-once debounce - pure stdlib.

Why: the LPRNet tail-character drop (bench finding 2026-09-26) produces
wrong-but-confident plates (chuanA8888 instead of chuanA88888) on single
frames. A physically present plate is recognized correctly on MOST frames,
so requiring the SAME string on `votes` consecutive cycles filters transient
misreads. Emission is debounced: one OK per confirmed plate until it goes
away (see ng_reset).

Used by edge_hub.RecogThread when --votes > 1; with --votes 1 the legacy
per-cycle broadcast behavior is kept untouched.
"""


class PlateVoter(object):
    """update(plate) -> plate string to emit, or None.

    plate = "" models a non-plate cycle (no_plate/low_conf/short).
    """

    def __init__(self, votes=2, ng_reset=3):
        self.votes = max(1, int(votes))
        self.ng_reset = max(1, int(ng_reset))
        self._pending = ""
        self._count = 0
        self._emitted = ""
        self._ng = 0

    def update(self, plate):
        if plate:
            self._ng = 0
            if plate == self._pending:
                self._count += 1
            else:
                self._pending = plate
                self._count = 1
            if self._count >= self.votes and plate != self._emitted:
                self._emitted = plate
                return plate
            return None
        self._pending = ""
        self._count = 0
        self._ng += 1
        if self._ng >= self.ng_reset:
            # plate gone for a while: re-arm so a returning vehicle with the
            # same plate triggers a fresh OK
            self._emitted = ""
        return None

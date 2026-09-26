# -*- coding: ascii -*-
"""Host unit tests for plate_vote.PlateVoter (pure stdlib)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import plate_vote  # noqa: E402


class TestPlateVoter(unittest.TestCase):
    def test_single_vote_emits_immediately(self):
        v = plate_vote.PlateVoter(votes=1)
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")

    def test_two_votes_need_consecutive(self):
        v = plate_vote.PlateVoter(votes=2)
        self.assertIsNone(v.update("chuanA88888"))      # 1st sighting: hold
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")  # confirmed

    def test_flicker_resets(self):
        v = plate_vote.PlateVoter(votes=2)
        self.assertIsNone(v.update("chuanA88888"))
        self.assertIsNone(v.update(""))                 # NG breaks the streak
        self.assertIsNone(v.update("chuanA88888"))      # back to 1
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")

    def test_transient_misread_filtered(self):
        # the bench case: mostly-correct frames with one wrong read
        v = plate_vote.PlateVoter(votes=2)
        self.assertIsNone(v.update("chuanA88888"))
        self.assertIsNone(v.update("chuanA8888"))       # wrong read, hold
        self.assertIsNone(v.update("chuanA88888"))      # streak broken, 1
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")

    def test_emit_once_per_presence(self):
        v = plate_vote.PlateVoter(votes=2)
        v.update("chuanA88888")
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")
        self.assertIsNone(v.update("chuanA88888"))      # already emitted
        self.assertIsNone(v.update("chuanA88888"))

    def test_new_plate_after_emitted(self):
        v = plate_vote.PlateVoter(votes=2)
        v.update("chuanA88888")
        v.update("chuanA88888")
        self.assertIsNone(v.update("yuB12345"))         # different plate, 1
        self.assertEqual(v.update("yuB12345"), "yuB12345")

    def test_rearm_after_ng_streak(self):
        v = plate_vote.PlateVoter(votes=2, ng_reset=3)
        v.update("chuanA88888")
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")
        for _ in range(3):                              # plate gone
            self.assertIsNone(v.update(""))
        # same plate returns: must vote again, then re-emits
        self.assertIsNone(v.update("chuanA88888"))
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")

    def test_ng_below_reset_keeps_emitted(self):
        v = plate_vote.PlateVoter(votes=2, ng_reset=3)
        v.update("chuanA88888")
        self.assertEqual(v.update("chuanA88888"), "chuanA88888")
        v.update("")
        v.update("")
        # plate still present after a 2-frame NG blink: no re-vote needed
        self.assertIsNone(v.update("chuanA88888"))


if __name__ == "__main__":
    unittest.main()

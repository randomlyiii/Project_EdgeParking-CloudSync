# -*- coding: ascii -*-
"""Host unit tests for lpr_decode (no third-party deps). Run anywhere:
    python -m unittest discover -s tests
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import lpr_decode  # noqa: E402

C = lpr_decode.CHARS
BLANK = lpr_decode.BLANK_INDEX
N = lpr_decode.NUM_CLASSES


def logits_from_seq(seq, t_pad=0, peak=10.0):
    """Build (T, C) score rows where each timestep peaks at seq[t]."""
    rows = []
    for cls in seq:
        row = [0.0] * N
        row[cls] = peak
        rows.append(row)
    for _ in range(t_pad):
        row = [0.0] * N
        row[BLANK] = peak
        rows.append(row)
    return rows


class TestTable(unittest.TestCase):
    def test_table_size(self):
        self.assertEqual(len(C), 67)
        self.assertEqual(BLANK, 67)
        self.assertEqual(N, 68)

    def test_provinces_present(self):
        for ch in ("\u4eac", "\u6d25", "\u6caa", "\u6e1d", "\u7ca4",
                   "\u4f7f", "\u9886"):
            self.assertIn(ch, C)

    def test_no_i_no_o(self):
        self.assertNotIn("I", C)
        self.assertNotIn("O", C)


class TestDecode(unittest.TestCase):
    def test_simple_plate(self):
        # jing A12345 -> class indices, blanks interleaved
        seq = [BLANK, C.index("\u4eac"), C.index("A"),
               C.index("1"), C.index("2"), C.index("3"),
               C.index("4"), C.index("5"), BLANK]
        plate, conf, _ = lpr_decode.ctc_greedy_decode(logits_from_seq(seq))
        self.assertEqual(plate, "\u4eacA12345")
        self.assertGreater(conf, 0.9)

    def test_dedup_consecutive(self):
        # duplicate consecutive same-class must collapse to one glyph
        seq = [C.index("A"), C.index("A"), C.index("A"), BLANK,
               C.index("B"), C.index("B")]
        plate, _conf, _ = lpr_decode.ctc_greedy_decode(logits_from_seq(seq))
        self.assertEqual(plate, "AB")

    def test_blank_runs_drop(self):
        seq = [BLANK, BLANK, C.index("9"), BLANK, BLANK, BLANK]
        plate, _conf, _ = lpr_decode.ctc_greedy_decode(logits_from_seq(seq))
        self.assertEqual(plate, "9")

    def test_all_blank_empty(self):
        plate, conf, seq = lpr_decode.ctc_greedy_decode(
            logits_from_seq([BLANK, BLANK, BLANK]))
        self.assertEqual(plate, "")
        self.assertGreater(conf, 0.9)   # model is sure there is nothing
        self.assertEqual(len(seq), 3)

    def test_class_time_orientation(self):
        # same answer whether fed as (T, C) or transposed (C, T)
        seq = [C.index("Z"), BLANK, C.index("8")]
        t_c = logits_from_seq(seq)
        c_t = [[t_c[t][c] for t in range(len(t_c))] for c in range(N)]
        p1, _c1, _ = lpr_decode.ctc_greedy_decode(t_c)
        p2, _c2, _ = lpr_decode.ctc_greedy_decode(c_t)
        self.assertEqual(p1, "Z8")
        self.assertEqual(p2, "Z8")

    def test_conf_reflects_peakiness(self):
        flat = [[1.0] * N for _ in range(4)]
        _p, conf, _ = lpr_decode.ctc_greedy_decode(flat)
        self.assertLess(conf, 0.5)
        sharp = logits_from_seq([C.index("A")] * 4, peak=50.0)
        _p, conf2, _ = lpr_decode.ctc_greedy_decode(sharp)
        self.assertGreater(conf2, 0.99)

    def test_row_too_short_raises(self):
        with self.assertRaises(ValueError):
            lpr_decode.ctc_greedy_decode([[0.0] * 10])

    def test_empty_input(self):
        self.assertEqual(lpr_decode.ctc_greedy_decode([]), ("", 0.0, []))


if __name__ == "__main__":
    unittest.main()

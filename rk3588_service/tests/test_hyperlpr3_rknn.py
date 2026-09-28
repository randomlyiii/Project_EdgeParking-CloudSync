# -*- coding: ascii -*-
"""Host unit tests for hyperlpr3_rknn.ctc_greedy (no numpy/rknn needed)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import hyperlpr3_rknn  # noqa: E402

TOKEN = ["blank", "'", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
         "A", "B", "C"]


def rows(seq, n_classes=16, peak=0.99):
    out = []
    for cls in seq:
        row = [0.01] * n_classes
        row[cls] = peak
        out.append(row)
    return out


class TestCtcGreedy(unittest.TestCase):
    def test_simple_plate_raw_conf(self):
        # sigmoid-baked output: conf must equal the mean of the raw maxes,
        # NOT a re-applied softmax (that bug squashed conf to ~0.03)
        seq = [0, TOKEN.index("A"), TOKEN.index("8"), 0, TOKEN.index("B")]
        plate, conf = hyperlpr3_rknn.ctc_greedy(rows(seq), TOKEN)
        self.assertEqual(plate, "A8B")
        self.assertAlmostEqual(conf, 0.99, places=5)

    def test_blank_and_dedup(self):
        seq = [TOKEN.index("A"), TOKEN.index("A"), 0, 0,
               TOKEN.index("B"), TOKEN.index("B")]
        plate, _ = hyperlpr3_rknn.ctc_greedy(rows(seq), TOKEN)
        self.assertEqual(plate, "AB")

    def test_conf_mean_over_kept_chars(self):
        seq = [TOKEN.index("A"), 0, TOKEN.index("B")]
        data = rows([0, 0, 0])                       # blank dominates t=1
        data[0][0] = 0.01
        data[0][TOKEN.index("A")] = 0.80
        data[2][0] = 0.01
        data[2][TOKEN.index("B")] = 0.60
        plate, conf = hyperlpr3_rknn.ctc_greedy(data, TOKEN)
        self.assertEqual(plate, "AB")
        self.assertAlmostEqual(conf, 0.70, places=5)

    def test_all_blank_empty(self):
        plate, conf = hyperlpr3_rknn.ctc_greedy(rows([0, 0, 0]), TOKEN)
        self.assertEqual(plate, "")
        self.assertEqual(conf, 0.0)

    def test_index_out_of_token_range_safe(self):
        seq = [len(TOKEN) + 2]          # beyond the charset: must not raise
        plate, conf = hyperlpr3_rknn.ctc_greedy(rows(seq, n_classes=25),
                                                TOKEN)
        self.assertEqual(plate, "?")
        self.assertGreater(conf, 0.9)


if __name__ == "__main__":
    unittest.main()

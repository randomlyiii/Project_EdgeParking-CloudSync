# -*- coding: ascii -*-
"""Host unit tests for yolo_decode (pure stdlib, no board needed)."""

import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import yolo_decode


def _grid(cx, cy, w, h, probs, num_anch=8400, num_cls=2):
    """Build a (1, 4+num_cls, num_anch) nested-list output with one hit."""
    grid = [[0.0] * num_anch for _ in range(4 + num_cls)]
    grid[0][0], grid[1][0] = cx, cy
    grid[2][0], grid[3][0] = w, h
    for c, p in enumerate(probs):
        grid[4 + c][0] = p
    return [grid]


class TestLetterbox(unittest.TestCase):
    def test_wide_image(self):
        # 613x396: width-limited -> scales to 640 wide, pads vertically
        scale, px, py, nw, nh = yolo_decode.letterbox(613, 396)
        self.assertAlmostEqual(scale, 640.0 / 613.0, places=4)
        self.assertEqual((px, nw), (0, 640))
        self.assertEqual(nh, int(round(396 * scale)))
        self.assertEqual(py, (640 - nh) // 2)
        self.assertGreater(py, 0)

    def test_round_trip(self):
        scale, px, py, nw, nh = yolo_decode.letterbox(613, 396)
        box = (100.0, 200.0, 300.0, 260.0)
        # forward: original -> 640 space (scale + pad), then back
        padded = (box[0] * scale + px, box[1] * scale + py,
                  box[2] * scale + px, box[3] * scale + py)
        back = yolo_decode.unletterbox(padded, scale, px, py)
        for a, b in zip(box, back):
            self.assertAlmostEqual(a, b, places=3)

    def test_tall_image(self):
        scale, px, py, nw, nh = yolo_decode.letterbox(396, 613)
        self.assertEqual(py, 0)
        self.assertGreater(px, 0)


class TestDecode(unittest.TestCase):
    def test_single_hit(self):
        outs = _grid(320.0, 320.0, 200.0, 100.0, [0.10, 0.92])
        keep = yolo_decode.decode(outs, conf_thres=0.25)
        self.assertEqual(len(keep), 1)
        x1, y1, x2, y2, conf, cls = keep[0]
        self.assertAlmostEqual(conf, 0.92, places=5)
        self.assertEqual(cls, 1)
        self.assertAlmostEqual(x1, 220.0, places=3)
        self.assertAlmostEqual(x2, 420.0, places=3)
        self.assertAlmostEqual(y1, 270.0, places=3)
        self.assertAlmostEqual(y2, 370.0, places=3)

    def test_conf_filter_and_nms(self):
        grid = _grid(320.0, 320.0, 200.0, 100.0, [0.1, 0.92])[0]
        # duplicate, weaker, heavily overlapping -> NMS must drop it
        grid[4][1], grid[5][1] = 0.1, 0.60
        grid[0][1], grid[1][1] = 325.0, 318.0
        grid[2][1], grid[3][1] = 190.0, 95.0
        # low-confidence junk anchor -> conf filter must drop it
        grid[4][2], grid[5][2] = 0.05, 0.10
        keep = yolo_decode.decode([grid], conf_thres=0.25)
        self.assertEqual(len(keep), 1)
        self.assertAlmostEqual(keep[0][4], 0.92, places=5)

    def test_two_distinct_boxes(self):
        grid = _grid(100.0, 100.0, 40.0, 20.0, [0.9, 0.1])[0]
        grid[0][1], grid[1][1] = 500.0, 500.0
        grid[2][1], grid[3][1] = 60.0, 30.0
        grid[4][1], grid[5][1] = 0.85, 0.05
        keep = yolo_decode.decode([grid], conf_thres=0.25, iou_thres=0.45)
        self.assertEqual(len(keep), 2)
        self.assertGreater(keep[0][4], keep[1][4])

    def test_max_det(self):
        grid = _grid(100.0, 100.0, 40.0, 20.0, [0.9, 0.1])[0]
        for a in range(1, 30):  # 29 far-apart boxes, all confident
            grid[0][a], grid[1][a] = 10.0 + (a % 8) * 70.0, 10.0 + (a // 8) * 70.0
            grid[2][a], grid[3][a] = 30.0, 15.0
            grid[4][a], grid[5][a] = 0.8, 0.05
        keep = yolo_decode.decode([grid], conf_thres=0.25, max_det=5)
        self.assertEqual(len(keep), 5)


class TestBoxMath(unittest.TestCase):
    def test_expand_clamps(self):
        box = yolo_decode.expand_box((10.0, 10.0, 50.0, 30.0), 0.1, 100, 100)
        self.assertEqual(box, (6.0, 8.0, 54.0, 32.0))
        edge = yolo_decode.expand_box((0.0, 0.0, 20.0, 20.0), 0.5, 100, 100)
        self.assertEqual(edge, (0.0, 0.0, 30.0, 30.0))

    def test_clamp_degenerate(self):
        self.assertIsNone(yolo_decode.clamp_box((5.0, 5.0, 6.0, 6.0), 100, 100))
        self.assertIsNone(yolo_decode.clamp_box((-10.0, -10.0, -5.0, -5.0), 100, 100))

    def test_iou(self):
        a = (0.0, 0.0, 10.0, 10.0)
        self.assertAlmostEqual(yolo_decode.iou(a, a), 1.0)
        self.assertAlmostEqual(yolo_decode.iou(a, (20.0, 20.0, 30.0, 30.0)), 0.0)
        self.assertAlmostEqual(
            yolo_decode.iou(a, (5.0, 0.0, 15.0, 10.0)), 1.0 / 3.0, places=4)


if __name__ == "__main__":
    unittest.main()

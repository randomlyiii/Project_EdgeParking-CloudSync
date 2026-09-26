# -*- coding: ascii -*-
"""YOLOv8 detection-head decode - pure python, zero third-party deps.

Shared by the two-stage bring-up harness (RK3588, board itest) and the host
unit tests. The deployed detector is we0091234/yolov8-plate yolov8s exported
via ultralytics (`format=onnx`, imgsz=640). Its output is (1, 4+nc, 8400):
cx, cy, w, h (pixels in the 640x640 letterboxed frame) followed by per-class
probabilities - the exported Detect head already applies sigmoid to the class
branch, so values here ARE probabilities. nc=2 (single/double plate) but the
decode below makes no assumption beyond >=1 class.

Boxes entering/leaving this module are in LETTERBOXED 640x640 coordinates;
use letterbox()/unletterbox() to map to/from the original image.
"""

IMG_SIZE = 640          # model input side
PAD_COLOR = 114         # yolo standard gray pad

import math


def letterbox(w, h, size=IMG_SIZE):
    """Fit (w, h) into size x size with gray padding.
    Returns (scale, pad_x, pad_y, new_w, new_h)."""
    scale = min(size / float(w), size / float(h))
    new_w = int(round(w * scale))
    new_h = int(round(h * scale))
    pad_x = (size - new_w) // 2
    pad_y = (size - new_h) // 2
    return scale, pad_x, pad_y, new_w, new_h


def unletterbox(box, scale, pad_x, pad_y):
    """(x1, y1, x2, y2) in 640 coords -> original image coords (floats)."""
    x1, y1, x2, y2 = box
    return ((x1 - pad_x) / scale, (y1 - pad_y) / scale,
            (x2 - pad_x) / scale, (y2 - pad_y) / scale)


def clamp_box(box, w, h):
    """Clamp (x1, y1, x2, y2) into [0, w] x [0, h], ensuring x2 > x1, y2 > y1.
    Returns None when the box degenerates to empty."""
    x1, y1, x2, y2 = box
    x1 = min(max(x1, 0.0), float(w))
    y1 = min(max(y1, 0.0), float(h))
    x2 = min(max(x2, 0.0), float(w))
    y2 = min(max(y2, 0.0), float(h))
    if x2 - x1 < 2.0 or y2 - y1 < 2.0:
        return None
    return (x1, y1, x2, y2)


def expand_box(box, margin, w, h):
    """Grow a box by `margin` fraction of its size on every side, clamped."""
    x1, y1, x2, y2 = box
    mx = (x2 - x1) * margin
    my = (y2 - y1) * margin
    return clamp_box((x1 - mx, y1 - my, x2 + mx, y2 + my), w, h)


def iou(a, b):
    """IoU of (x1, y1, x2, y2, ...) boxes a and b (first 4 fields used)."""
    ax1, ay1, ax2, ay2 = a[:4]
    bx1, by1, bx2, by2 = b[:4]
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    if inter <= 0.0:
        return 0.0
    area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    return inter / (area_a + area_b - inter)


def decode(outputs, conf_thres=0.25, iou_thres=0.45, max_det=10):
    """Decode raw yolov8 outputs -> kept detections, best conf first.

    outputs: nested lists, (1, 4+nc, 8400) or (4+nc, 8400).
    Returns a list of (x1, y1, x2, y2, conf, cls) in letterboxed IMG_SIZE
    coordinates. NMS is class-agnostic (all detections are plates anyway).
    """
    grid = outputs[0] if len(outputs) == 1 else outputs
    num_attr = len(grid)
    num_cls = num_attr - 4
    num_anch = len(grid[0])
    cands = []
    for a in range(num_anch):
        w = grid[2][a]
        if w <= 0.0 or w >= IMG_SIZE * 2:
            continue
        h = grid[3][a]
        if h <= 0.0 or h >= IMG_SIZE * 2:
            continue
        best_s, best_c = -1.0, 0
        for c in range(num_cls):
            s = grid[4 + c][a]
            if s > best_s:
                best_s, best_c = s, c
        if best_s < conf_thres:
            continue
        cx, cy = grid[0][a], grid[1][a]
        cands.append((cx - w / 2.0, cy - h / 2.0, cx + w / 2.0, cy + h / 2.0,
                      best_s, best_c))
    cands.sort(key=lambda b: -b[4])
    keep = []
    for b in cands:
        if len(keep) >= max_det:
            break
        if all(iou(b, k) <= iou_thres for k in keep):
            keep.append(b)
    return keep

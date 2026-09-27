# -*- coding: ascii -*-
"""On-board A/B test: HyperLPR3 det+rec on RKNN (NPU) vs the installed CPU
onnxruntime engine (LicensePlateCatcher, 320 det default).

Decode semantics come from hyperlpr3.inference.multitask_detect (the class the
production pipeline actually uses): the fused (1, N, 15) output is decoded
RAW - no sigmoid / no anchors / no grid - channels are
[0:4 xywh][4 obj][5:13 landmarks][13:15 class scores]. "sim" = sigmoid baked
into the exported model. The crop is perspective-rectified with the 4
landmarks (get_rotate_crop_image) - that is where rotation invariance lives.

Usage: python3 itest_hyperlpr3_rknn.py <img_dir> [det.rknn] [rec.rknn]
"""
import glob
import os
import sys
import time

import cv2
import numpy as np

# common modules only: inference.* class bodies use a broken @cost decorator
# at import time, but multitask_detect's functions are safe to import
from hyperlpr3.common.tools_process import get_rotate_crop_image
from hyperlpr3.common.tokenize import token
from hyperlpr3.inference.multitask_detect import (letter_box, post_precessing,
                                                  nms)

DET_IN = 320          # CPU default (DETECT_LEVEL_LOW); 640 optional
REC_HW = (48, 160)
DET_RKNN = sys.argv[2] if len(sys.argv) > 2 else "y5fu_320x.rknn"
REC_RKNN = sys.argv[3] if len(sys.argv) > 3 else "rpv3_mdict_160_r3.rknn"
IMG_DIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/plates"


def decode_rec(out):
    """CTC greedy; rpv3 output is (1, T, 78) with C = len(token) = 78
    (the earlier "65-token" note undercounted). Orient by the charset dim."""
    pred = np.array(out[0] if isinstance(out, list) else out)
    if pred.ndim == 3:
        pred = pred.squeeze(0)
    if pred.shape[0] == len(token):      # (C, T) -> (T, C)
        pred = pred.T
    e = np.exp(pred - pred.max(axis=1, keepdims=True))
    p = e / e.sum(axis=1, keepdims=True)
    seq = np.argmax(pred, axis=1)
    probs = np.max(p, axis=1)
    chars, confs = [], []
    prev = -1
    for t, idx in enumerate(seq):
        if idx == 0 or idx == prev:
            prev = idx
            continue
        prev = idx
        chars.append(token[int(idx)])
        confs.append(float(probs[t]))
    return "".join(chars), (float(np.mean(confs)) if confs else 0.0)


def rec_preprocess(crop):
    """encode_images geometry on the raw BGR crop, NHWC uint8 for rknn."""
    h, w = crop.shape[:2]
    rw = min(max(int(round(48 * (w / float(h)))), 48), 160)
    resized = cv2.resize(crop, (rw, 48))
    canvas = np.full((48, 160, 3), 127, dtype=np.uint8)   # 127 ~ 0 normalized
    canvas[:, 0:rw, :] = resized
    x = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
    return np.expand_dims(x, 0).astype(np.uint8)           # (1,48,160,3)


def run_image(det, rec, bgr):
    """Returns (plate, conf, box, det_ms, rec_ms)."""
    lb, r, left, top = letter_box(bgr, (DET_IN, DET_IN))
    x = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB)
    x = np.expand_dims(x, 0).astype(np.uint8)
    t0 = time.time()
    dets = det.inference(inputs=[x])[0]
    det_ms = (time.time() - t0) * 1000
    dets = np.array(dets).reshape(-1, 15)
    outs = post_precessing(dets.reshape(1, -1, 15), r, left, top, 0.25, 0.5)
    if outs is None or len(outs) == 0:
        return "", 0.0, None, det_ms, 0.0
    out = outs[0]
    lm = out[5:13].reshape(4, 2).astype(np.float32)
    pad = get_rotate_crop_image(bgr, lm)
    if int(out[13]) == 1:                      # double layer
        h = pad.shape[0]
        line = int(h * 0.4)
        p1, _c1 = decode_rec(rec.inference(inputs=[rec_preprocess(pad[:line])]))
        p2, _c2 = decode_rec(rec.inference(inputs=[rec_preprocess(pad[line:])]))
        plate = p1 + p2
        conf = (_c1 + _c2) / 2
        rec_ms = 0.0
    else:
        t0 = time.time()
        o = rec.inference(inputs=[rec_preprocess(pad)])
        rec_ms = (time.time() - t0) * 1000
        plate, conf = decode_rec(o)
    return plate, conf, [int(v) for v in out[:4]], det_ms, rec_ms


def cpu_reference(catcher, bgr):
    t0 = time.time()
    res = catcher(bgr)
    ms = (time.time() - t0) * 1000
    if not res:
        return "", ms
    return res[0][0], ms


def main():
    from rknnlite.api import RKNNLite
    det = RKNNLite()
    assert det.load_rknn(DET_RKNN) == 0
    assert det.init_runtime() == 0
    rec = RKNNLite()
    assert rec.load_rknn(REC_RKNN) == 0
    assert rec.init_runtime() == 0

    from hyperlpr3 import LicensePlateCatcher
    catcher = LicensePlateCatcher()

    imgs = sorted(glob.glob(os.path.join(IMG_DIR, "*.jpg")) +
                  glob.glob(os.path.join(IMG_DIR, "*.png")))
    print("found %d images; det=%s (%d) rec=%s" %
          (len(imgs), DET_RKNN, DET_IN, REC_RKNN), flush=True)
    n_match = 0
    n_cpu = 0
    for path in imgs:
        bgr = cv2.imread(path, cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        plate, conf, box, det_ms, rec_ms = run_image(det, rec, bgr)
        cpu_plate, cpu_ms = cpu_reference(catcher, bgr)
        if cpu_plate:
            n_cpu += 1
        ok = (plate == cpu_plate) and plate != ""
        if ok:
            n_match += 1
        print("%-24s RKNN: %-10s conf=%.3f det=%4.0fms rec=%4.0fms | CPU: "
              "%-10s %4.0fms | %s" % (os.path.basename(path), plate, conf,
                                      det_ms, rec_ms, cpu_plate, cpu_ms,
                                      "OK" if ok else "DIFF"), flush=True)
    print("MATCH %d/%d (cpu got %d)" % (n_match, len(imgs), n_cpu), flush=True)
    det.release()
    rec.release()


if __name__ == "__main__":
    main()

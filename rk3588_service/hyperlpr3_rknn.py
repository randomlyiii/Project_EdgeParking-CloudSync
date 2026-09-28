# -*- coding: ascii -*-
"""HyperLPR3 on RKNN (NPU): y5fu_320x detect + rpv3_mdict_160_r3 recognize.

Same plate() contract as lpr_server.HyperLpr3Recognizer:
    plate(jpeg_bytes) -> (plate, conf, info) | None
info = {"box": [x1,y1,x2,y2], "ptype": int, "engine": "hyperlpr3rknn"};
no detection = ("", 0.0, {"stage": "no_det"}).

NPU det+rec ~18ms/frame (320 variant). Acceptance 2026-09-27: 8/8 match
with the CPU engine on the torture set (RESULTS.md). Inputs are NHWC uint8
(mean/std baked into the exported models): det mean 0/std 255, rec mean/std
127.5, BGR->RGB done here.

Decode facts (mined from the hyperlpr3 package - do NOT re-derive):
- det fused output (1, N, 15), anchor-free, sigmoid BAKED into the export
  ("sim"): obj = raw channel 4 > 0.25, landmarks d[5:13], layer class
  d[13:15] (0 single / 1 double). Decode = post_precessing + NMS.
- rec output (1, T, 78): CTC greedy, blank index 0, charset from
  hyperlpr3.common.tokenize (78 tokens).
- conf = MEAN of the per-char max RAW rec values. The sim export is
  already sigmoid-ed: applying softmax AGAIN squashes conf to ~0.03 and
  would never pass min_conf (this bit cost one debug round).
"""

import threading

try:
    import numpy as np
    import cv2
except Exception:                       # pragma: no cover - host without deps
    np = None
    cv2 = None

DET_IN = 320
REC_H = 48
REC_W = 160
BOX_THR = 0.25
IOU_THR = 0.5
DOUBLE_LAYER = 1

DEFAULT_DET = "/root/models/y5fu_320x.rknn"
DEFAULT_REC = "/root/models/rpv3_mdict_160_r3.rknn"


def ctc_greedy(pred, token):
    """CTC greedy decode on (T, C) raw rows.

    pred: sequence of per-timestep score rows (lists or arrays).
    token: charset list; index 0 is the blank.
    Returns (plate, conf) with conf = mean per-kept-char max raw value.
    """
    chars = []
    confs = []
    prev = -1
    for row in pred:
        best_i = 0
        best_v = row[0]
        for i in range(1, len(row)):
            if row[i] > best_v:
                best_v = row[i]
                best_i = i
        if best_i == 0 or best_i == prev:
            prev = best_i
            continue
        prev = best_i
        chars.append(token[best_i] if best_i < len(token) else "?")
        confs.append(float(best_v))
    if not confs:
        return "", 0.0
    return "".join(chars), sum(confs) / len(confs)


class HyperLpr3RknnRecognizer(object):
    """RKNN NPU HyperLPR3. Lazy deps: importing this module is host-safe."""

    def __init__(self, det_path=DEFAULT_DET, rec_path=DEFAULT_REC):
        if np is None or cv2 is None:
            raise RuntimeError("numpy/cv2 not installed")
        try:
            from rknnlite.api import RKNNLite
            from hyperlpr3.common.tokenize import token
            from hyperlpr3.inference.multitask_detect import (
                letter_box, post_precessing)
            from hyperlpr3.common.tools_process import get_rotate_crop_image
        except ImportError as exc:
            raise RuntimeError("rknnlite/hyperlpr3 not installed (%s)"
                               % (exc,))
        self._token = token
        self._letter_box = letter_box
        self._post = post_precessing
        self._crop = get_rotate_crop_image

        self._det = RKNNLite()
        if self._det.load_rknn(det_path) != 0:
            raise RuntimeError("load_rknn failed: %s" % det_path)
        if self._det.init_runtime() != 0:
            raise RuntimeError("init_runtime failed: %s" % det_path)
        self._rec = RKNNLite()
        if self._rec.load_rknn(rec_path) != 0:
            raise RuntimeError("load_rknn failed: %s" % rec_path)
        if self._rec.init_runtime() != 0:
            raise RuntimeError("init_runtime failed: %s" % rec_path)
        self._lock = threading.Lock()

    def _rec_one(self, crop):
        h, w = crop.shape[:2]
        rw = int(round(REC_H * (w / float(h))))
        rw = min(max(rw, 48), REC_W)
        resized = cv2.resize(crop, (rw, REC_H))
        # pad value 127 ~ 0 after the baked (x-127.5)/127.5 normalization
        canvas = np.full((REC_H, REC_W, 3), 127, dtype=np.uint8)
        canvas[:, 0:rw, :] = resized
        x = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
        x = np.expand_dims(x, 0).astype(np.uint8)      # (1,48,160,3) NHWC
        out = self._rec.inference(inputs=[x])[0]
        pred = np.array(out).squeeze(0)                # (T, 78)
        if pred.shape[0] == len(self._token):          # safety: orient (C,T)
            pred = pred.T
        return ctc_greedy(pred.tolist(), self._token)

    def plate(self, jpeg_bytes):
        arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if img is None:
            return None
        with self._lock:
            lb, r, left, top = self._letter_box(img, (DET_IN, DET_IN))
            x = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB)
            x = np.expand_dims(x, 0).astype(np.uint8)  # (1,320,320,3) NHWC
            dets = self._det.inference(inputs=[x])[0]
            outs = self._post(np.asarray(dets).reshape(1, -1, 15),
                              r, left, top, BOX_THR, IOU_THR)
            if outs is None or len(outs) == 0:
                return "", 0.0, {"stage": "no_det"}
            o = outs[0]
            lm = o[5:13].reshape(4, 2).astype(np.float32)
            pad = self._crop(img, lm)
            if int(o[13]) == DOUBLE_LAYER:
                h = pad.shape[0]
                line = int(h * 0.4)
                p1, c1 = self._rec_one(pad[:line])
                p2, c2 = self._rec_one(pad[line:])
                plate = p1 + p2
                conf = (c1 + c2) / 2.0
            else:
                plate, conf = self._rec_one(pad)
        return plate, conf, {
            "box": [int(v) for v in o[:4]],
            "ptype": int(o[13]),
            "engine": "hyperlpr3rknn",
        }

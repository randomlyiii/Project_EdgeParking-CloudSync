# -*- coding: ascii -*-
"""LPRNet edge recognition server - runs on the RK3588 (NPU via RKNN Lite).

HTTP interface (docs/protocols.md section 5):
  GET  /health     -> {"ok": true|false, "classes": 68, "model": "<path>"}
  POST /recognize  body = raw JPEG bytes
      200 {"plate": "<plate>", "conf": 0.93, "ms": 12.3}
      200 {"error": "no_plate"|"low_conf", "conf": 0.31, "ms": 12.3}
      400 {"error": "bad_length"|"bad_image"}
      503 {"error": "rknn unavailable"}

The RKNN runtime object is not thread-safe: all inferences go through one
lock. Heavy deps (numpy/cv2/rknnlite) import lazily so the module also loads
on a plain host for unit tests with a stub recognizer.
"""

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import lpr_decode

try:
    import numpy as np
    import cv2
except Exception:                       # pragma: no cover - host without deps
    np = None
    cv2 = None

try:
    from rknnlite.api import RKNNLite
except Exception:                       # pragma: no cover - host without deps
    RKNNLite = None

DEFAULT_PORT = 8088
DEFAULT_MODEL = "/root/models/lprnet.rknn"
MAX_BODY_BYTES = 1024 * 1024


class Recognizer(object):
    """Real RK3588 NPU recognizer. logits() returns nested lists (C, T).

    roi: optional (x, y, w, h) in relative 0..1 coordinates - crop the plate
    region BEFORE the 94x24 resize. LPRNet expects a plate-crop-like input;
    feeding it a whole far-away frame shrinks the plate past recognition
    (the 2026-09-26 low-accuracy report). Default (0,0,1,1) = no crop."""

    def __init__(self, model_path, roi=None):
        if RKNNLite is None:
            raise RuntimeError("rknnlite not installed")
        if np is None or cv2 is None:
            raise RuntimeError("numpy/cv2 not installed")
        self._roi = self._norm_roi(roi)
        self._rknn = RKNNLite()
        ret = self._rknn.load_rknn(model_path)
        if ret != 0:
            raise RuntimeError("load_rknn failed: %d" % ret)
        # No target= arg: on-device runtime init. Passing target='rk3588'
        # makes lite2 enter remote/adb mode, which fails on the board
        # (verified 2026-09-26, lite2 2.3.2 + librknnrt 2.1.0).
        ret = self._rknn.init_runtime()
        if ret != 0:
            raise RuntimeError("init_runtime failed: %d" % ret)
        self._lock = threading.Lock()

    @staticmethod
    def _norm_roi(roi):
        if not roi:
            return None
        x, y, w, h = (float(v) for v in roi)
        if w <= 0.0 or h <= 0.0:
            return None                 # degenerate crop = no crop
        x = min(max(x, 0.0), 0.99)
        y = min(max(y, 0.0), 0.99)
        w = min(max(w, 0.01), 1.0 - x)
        h = min(max(h, 0.01), 1.0 - y)
        return (x, y, w, h)

    def logits(self, jpeg_bytes):
        arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)   # BGR, matches training
        if img is None:
            return None
        if self._roi:
            H, W = img.shape[:2]
            x, y, w, h = self._roi
            x0 = int(x * W)
            y0 = int(y * H)
            x1 = min(x0 + int(w * W), W)
            y1 = min(y0 + int(h * H), H)
            if x1 - x0 >= 8 and y1 - y0 >= 8:
                img = img[y0:y1, x0:x1]
        img = cv2.resize(img, (94, 24))
        # NHWC uint8. The exported model has mean/std (127.5/127.5) baked
        # in, so the runtime applies (x-127.5)/127.5 itself. Verified on
        # board 2026-09-26: chuan A 88888, conf 0.986, 2.5 ms.
        img = img[np.newaxis, :].astype(np.uint8)
        with self._lock:
            outs = self._rknn.inference(inputs=[img])
        return outs[0][0].tolist()                  # (68, 18)


def make_server(host, port, recognizer, model_path,
                min_conf=0.70, max_bytes=MAX_BODY_BYTES):
    """Build (not start) the ThreadingHTTPServer. Split out for unit tests."""

    class Handler(BaseHTTPRequestHandler):
        server_version = "LprServer/1.0"
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass                                # we log explicitly below

        def _send_json(self, code, obj):
            body = json.dumps(obj, ensure_ascii=True).encode("ascii")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if urlparse(self.path).path != "/health":
                self._send_json(404, {"error": "not found"})
                return
            self._send_json(200, {
                "ok": recognizer is not None,
                "classes": lpr_decode.NUM_CLASSES,
                "model": model_path,
            })

        def do_POST(self):
            if urlparse(self.path).path != "/recognize":
                self._send_json(404, {"error": "not found"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            if length <= 0 or length > max_bytes:
                self._send_json(400, {"error": "bad_length"})
                return
            body = self.rfile.read(length)
            if len(body) != length:
                self._send_json(400, {"error": "bad_length"})
                return
            if recognizer is None:
                self._send_json(503, {"error": "rknn unavailable"})
                return
            t0 = time.time()
            try:
                logits = recognizer.logits(body)
            except Exception as exc:            # pragma: no cover - defensive
                print("recog internal: %r" % (exc,), file=sys.stderr, flush=True)
                self._send_json(500, {"error": "internal"})
                return
            if logits is None:
                self._send_json(400, {"error": "bad_image"})
                return
            plate, conf, _seq = lpr_decode.ctc_greedy_decode(logits)
            ms = round((time.time() - t0) * 1000.0, 1)
            if not plate:
                print("recog fail reason=no_plate conf=%.3f ms=%.1f"
                      % (conf, ms), flush=True)
                self._send_json(200, {"error": "no_plate",
                                      "conf": round(conf, 4), "ms": ms})
            elif conf < min_conf:
                print("recog fail reason=low_conf conf=%.3f ms=%.1f"
                      % (conf, ms), flush=True)
                self._send_json(200, {"error": "low_conf",
                                      "conf": round(conf, 4), "ms": ms})
            else:
                print("recog ok conf=%.3f ms=%.1f chars=%d"
                      % (conf, ms, len(plate)), flush=True)
                self._send_json(200, {"plate": plate,
                                      "conf": round(conf, 4), "ms": ms})

    srv = ThreadingHTTPServer((host, port), Handler)
    srv.daemon_threads = True
    return srv


def main(argv=None):
    ap = argparse.ArgumentParser(description="LPRNet edge recognition server")
    ap.add_argument("--host", default="192.168.10.2",
                    help="listen address; default = the MP157 link address "
                         "only, so the unauthenticated endpoint is not "
                         "reachable from other interfaces")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--min-conf", type=float, default=0.70)
    args = ap.parse_args(argv)

    recognizer = None
    try:
        recognizer = Recognizer(args.model)
        print("lpr server: model loaded: %s" % args.model, flush=True)
    except Exception as exc:
        print("lpr server: WARNING recognizer unavailable: %r" % (exc,),
              file=sys.stderr, flush=True)
    # Bind with retry: the static address may not exist yet at boot.
    srv = None
    for _attempt in range(30):
        try:
            srv = make_server(args.host, args.port, recognizer, args.model,
                              min_conf=args.min_conf)
            break
        except OSError as exc:
            print("lpr server: cannot bind %s:%d (%s) - retry in 2s"
                  % (args.host, args.port, exc), flush=True)
            time.sleep(2)
    if srv is None:
        print("lpr server: bind failed, giving up", file=sys.stderr,
              flush=True)
        sys.exit(1)
    print("lpr server: listening on %s:%d (min_conf=%.2f)"
          % (args.host, args.port, args.min_conf), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()

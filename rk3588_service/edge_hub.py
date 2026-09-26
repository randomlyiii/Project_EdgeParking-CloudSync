# -*- coding: ascii -*-
"""edge_hub - RK3588 edge node hub (K210 reader + LPRNet + TCP relay).

Topology (docs/protocols.md section 5):
    K210 --USB CDC--> edge_hub (this process, on RK3588)
    edge_hub --TCP :8089--> MP157 park_ui (k210_link, mode=tcp)

The hub relays the K210 console line stream verbatim (K2:IMG/K2:END frames,
[stat]/[BOOT] logs) so MP157 keeps its existing parser untouched, and injects
K2:OK:/K2:NG: result lines produced by periodic LPRNet recognition on the
newest assembled JPEG - the same uplink contract the old on-K210 recognizer
had (source 0xC2/0xC3 console form).

Threads:
  reader  - tty -> per-line broadcast + JPEG reassembly -> latest-frame slot
  hub     - ThreadingTCPServer; one queue per client; slow clients dropped
  recog   - every --period ms: newest fresh JPEG -> RKNN -> result line

Only stdlib + lpr_server.Recognizer (numpy/cv2/rknnlite) are used.
"""

import argparse
import base64
import binascii
import json
import os
import queue
import select
import socket
import sys
import threading
import time
from socketserver import StreamRequestHandler, ThreadingTCPServer

import lpr_decode
from lpr_server import Recognizer, TwoStageRecognizer

DEFAULT_TTY = "/dev/ttyACM0"
DEFAULT_PORT = 8089
DEFAULT_PERIOD_MS = 3000
DEFAULT_FRESH_MS = 6500


class FrameAssembler(object):
    """Reassembles K2:IMG/K2:END base64 chunks into one JPEG per frame.

    Mirrors the caps of core1_ui k210_link (64 chunks x 8 KB): a breach
    drops the partial frame and resyncs, so a lost K2:END can never strand
    chunks forever.
    """

    MAX_CHUNKS = 64
    MAX_CHUNK_CHARS = 8 * 1024

    def __init__(self):
        self._chunks = {}

    def feed_line(self, line):
        """Feed one console line (no newline). Returns JPEG bytes or None."""
        if line.startswith("K2:IMG:"):
            rest = line[len("K2:IMG:"):]
            off, sep, b64 = rest.partition(":")
            if not sep or not off or not b64:
                return None
            try:
                off_i = int(off)
            except ValueError:
                return None
            if (len(b64) > self.MAX_CHUNK_CHARS or
                    len(self._chunks) >= self.MAX_CHUNKS):
                self._chunks.clear()
            self._chunks[off_i] = b64
            return None
        if line.startswith("K2:END:"):
            try:
                want = int(line[len("K2:END:"):])
            except ValueError:
                self._chunks.clear()
                return None
            data = "".join(self._chunks[k] for k in sorted(self._chunks))
            self._chunks.clear()
            if want > 0 and data and len(data) == want:
                try:
                    return binascii.a2b_base64(data.encode("ascii"))
                except (binascii.Error, ValueError, UnicodeEncodeError):
                    return None
            return None
        return None


class LineHub(object):
    """Fan-out of text lines to connected TCP clients.

    One queue per client; a client that falls behind (queue full) is dropped
    and is expected to reconnect. Not thread-safe by itself - guarded by
    hub lock in broadcast/add/remove.
    """

    def __init__(self, max_queue=256):
        self._lock = threading.Lock()
        self._clients = []          # list of queue.Queue
        self._max_queue = max_queue

    def add(self, q):
        with self._lock:
            self._clients.append(q)

    def remove(self, q):
        with self._lock:
            if q in self._clients:
                self._clients.remove(q)

    def broadcast(self, line):
        payload = (line + "\n").encode("ascii", "replace")
        with self._lock:
            clients = list(self._clients)
        dead = []
        for q in clients:
            try:
                q.put_nowait(payload)
            except queue.Full:
                dead.append(q)
        for q in dead:
            self.remove(q)

    def client_count(self):
        with self._lock:
            return len(self._clients)


def make_handler(hub):
    class Handler(StreamRequestHandler):
        def handle(self):
            q = queue.Queue(maxsize=hub._max_queue)
            hub.add(q)
            print("hub: client %s:%d connected (%d)"
                  % (self.client_address[0], self.client_address[1],
                     hub.client_count()), flush=True)
            try:
                while True:
                    payload = q.get()
                    self.wfile.write(payload)
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                hub.remove(q)
                print("hub: client %s:%d gone (%d left)"
                      % (self.client_address[0], self.client_address[1],
                         hub.client_count()), flush=True)

    return Handler


def open_tty(dev, baud):
    """Raw-mode tty, stdlib only (same recipe as k210_preview_rx_text.py)."""
    import termios
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
              termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG |
              termios.IEXTEN)
    bc = getattr(termios, "B%d" % baud, None) or termios.B115200
    a[4] = a[5] = bc
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def jpeg_to_k2_lines(jpeg, chunk=900):
    """Split a JPEG into the K2:IMG/K2:END console line stream (same framing
    the K210 firmware emits), so a local camera feeds park_ui UNCHANGED."""
    b64 = base64.b64encode(jpeg).decode("ascii")
    lines = ["K2:IMG:%d:%s" % (off, b64[off:off + chunk])
             for off in range(0, len(b64), chunk)]
    lines.append("K2:END:%d" % len(b64))
    return lines


class CameraThread(threading.Thread):
    """Local V4L2 camera source (--source /dev/videoN).

    Replaces the K210 console relay: grabs MJPEG frames from a UVC camera,
    feeds the recognizer slot and broadcasts the SAME K2 line stream, so
    MP157 park_ui needs no change. Preview fps to the LCD decouples from the
    camera fps (relay_fps knob); recognition eats the freshest frame.
    """

    def __init__(self, hub, latest, device, width, height, cam_fps,
                 relay_fps, jpeg_quality, roi=None):
        super(CameraThread, self).__init__(daemon=True)
        self._hub = hub
        self._latest = latest
        self._dev = device
        self._w = width
        self._h = height
        self._camFps = max(1, cam_fps)
        self._relayEvery = max(1, int(round(float(cam_fps) /
                                            max(1, relay_fps))))
        self._quality = jpeg_quality
        # draw the ROI rectangle into the RELAYED frame (not the one the
        # recognizer eats), so the LCD shows exactly what the crop covers.
        self._drawBox = None
        if roi and tuple(roi) != (0.0, 0.0, 1.0, 1.0):
            x = min(max(float(roi[0]), 0.0), 0.99)
            y = min(max(float(roi[1]), 0.0), 0.99)
            w = min(max(float(roi[2]), 0.01), 1.0 - x)
            h = min(max(float(roi[3]), 0.01), 1.0 - y)
            self._drawBox = (x, y, w, h)
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        import cv2
        while not self._stop.is_set():
            cap = cv2.VideoCapture(self._dev, cv2.CAP_V4L2)
            if not cap.isOpened():
                print("cam: cannot open %s - retry in 2s" % self._dev,
                      flush=True)
                self._hub.broadcast("[hub] cam link DOWN")
                self._stop.wait(2.0)
                continue
            cap.set(cv2.CAP_PROP_FOURCC,
                    cv2.VideoWriter_fourcc(*"MJPG"))
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, self._w)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self._h)
            cap.set(cv2.CAP_PROP_FPS, self._camFps)
            print("cam: %s open %dx%d@%dfps relay every %d frame(s)"
                  % (self._dev, self._w, self._h, self._camFps,
                     self._relayEvery), flush=True)
            got = 0
            sent = 0
            statAt = time.time()
            try:
                while not self._stop.is_set():
                    ok, frame = cap.read()
                    if not ok:
                        break
                    got += 1
                    r, buf = cv2.imencode(
                        ".jpg", frame,
                        [cv2.IMWRITE_JPEG_QUALITY, self._quality])
                    if not r:
                        continue
                    jpeg = buf.tobytes()
                    self._latest["jpeg"] = jpeg
                    self._latest["at"] = int(time.time() * 1000)
                    if got % self._relayEvery == 0:
                        sent += 1
                        relay_jpeg = jpeg
                        if self._drawBox is not None:
                            x, y, w, h = self._drawBox
                            x0 = int(x * self._w)
                            y0 = int(y * self._h)
                            x1 = min(x0 + int(w * self._w), self._w - 1)
                            y1 = min(y0 + int(h * self._h), self._h - 1)
                            vis = frame.copy()
                            cv2.rectangle(vis, (x0, y0), (x1, y1),
                                          (0, 255, 255), 2)
                            rv, vbuf = cv2.imencode(
                                ".jpg", vis,
                                [cv2.IMWRITE_JPEG_QUALITY, self._quality])
                            if rv:
                                relay_jpeg = vbuf.tobytes()
                        for line in jpeg_to_k2_lines(relay_jpeg):
                            self._hub.broadcast(line)
                    now = time.time()
                    if now - statAt >= 2.0:
                        self._hub.broadcast(
                            "[stat] fps=%d jpeg=%dB cam=%dx%d"
                            % (int(round(got / (now - statAt))),
                               len(jpeg), self._w, self._h))
                        got = 0
                        statAt = now
            finally:
                cap.release()
            print("cam: lost %s - retry in 2s" % self._dev, flush=True)
            self._hub.broadcast("[hub] cam link DOWN")
            self._stop.wait(2.0)


class ReaderThread(threading.Thread):
    """tty -> broadcast lines; assembled JPEGs -> latest slot."""

    def __init__(self, hub, latest, dev, baud):
        super(ReaderThread, self).__init__(daemon=True)
        self._hub = hub
        self._latest = latest        # dict slot: {"jpeg": bytes, "at": ms}
        self._dev = dev
        self._baud = baud
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        fd = -1
        buf = b""
        asm = FrameAssembler()
        while not self._stop.is_set():
            if fd < 0:
                try:
                    fd = open_tty(self._dev, self._baud)
                    print("reader: %s open" % self._dev, flush=True)
                except OSError as exc:
                    print("reader: cannot open %s (%s) - retry in 2s"
                          % (self._dev, exc), flush=True)
                    self._hub.broadcast("[hub] k210 link DOWN")
                    self._stop.wait(2.0)
                    continue
            try:
                r, _, _ = select.select([fd], [], [], 0.2)
            except (OSError, ValueError):
                os.close(fd)
                fd = -1
                continue
            if not r:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                os.close(fd)
                fd = -1
                continue
            if not data:
                os.close(fd)
                fd = -1
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip("\r").strip()
                if not line:
                    continue
                self._hub.broadcast(line)
                jpeg = asm.feed_line(line)
                if jpeg:
                    self._latest["jpeg"] = jpeg
                    self._latest["at"] = int(time.time() * 1000)
        if fd >= 0:
            os.close(fd)


class RecogThread(threading.Thread):
    """Periodic recognition on the newest fresh JPEG; result lines -> hub.

    The recognizer exposes plate(jpeg) -> (plate, conf, info) | None
    (lpr_server.Recognizer or TwoStageRecognizer).
    """

    def __init__(self, hub, latest, recognizer, period_ms, fresh_ms,
                 min_conf, min_chars=5):
        super(RecogThread, self).__init__(daemon=True)
        self._hub = hub
        self._latest = latest
        self._recognizer = recognizer
        self._period = period_ms / 1000.0
        self._fresh = fresh_ms
        self._min_conf = min_conf
        # Chinese plates are 7-8 chars; anything shorter is a background
        # false positive no matter how confident the net is (seen live on
        # the bench: "jin A" at conf 1.000 on a bare wall, 2026-09-26).
        self._min_chars = min_chars
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        while not self._stop.wait(self._period):
            jpeg = self._latest.get("jpeg")
            at = self._latest.get("at", 0)
            if not jpeg or int(time.time() * 1000) - at > self._fresh:
                continue
            try:
                t0 = time.time()
                result = self._recognizer.plate(jpeg)
            except Exception as exc:
                print("recog: inference error %r" % (exc,), file=sys.stderr,
                      flush=True)
                continue
            if result is None:
                continue
            plate, conf, info = result
            ms = int((time.time() - t0) * 1000)
            candidate = ""
            if not plate:
                self._hub.broadcast("K2:NG:no_plate")
                print("recog: no_plate conf=%.3f ms=%d"
                      % (conf, ms), flush=True)
            elif len(plate) < self._min_chars:
                self._hub.broadcast("K2:NG:no_plate")
                print("recog: short_plate chars=%d conf=%.3f ms=%d"
                      % (len(plate), conf, ms), flush=True)
            elif conf < self._min_conf:
                self._hub.broadcast("K2:NG:low_conf")
                print("recog: low_conf conf=%.3f ms=%d"
                      % (conf, ms), flush=True)
            else:
                candidate = plate
            if candidate:
                self._emit_ok(candidate, conf, ms, info)

    def _emit_ok(self, plate, conf, ms, info):
        payload = json.dumps(
            {"plate": plate, "confidence": round(conf, 4)},
            ensure_ascii=True)
        self._hub.broadcast("K2:OK:" + payload)
        extra = ""
        if info.get("box"):
            extra = " box=%s" % (info["box"],)
        if info.get("det_conf") is not None:
            extra += " det=%.2f" % info["det_conf"]
        print("recog: OK conf=%.3f ms=%d chars=%d%s"
              % (conf, ms, len(plate), extra), flush=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description="RK3588 edge hub")
    ap.add_argument("--source", default=DEFAULT_TTY,
                    help="/dev/ttyACM0 = K210 console relay (default); "
                         "/dev/videoN = local V4L2/UVC camera - emits the "
                         "same K2 line stream, park_ui is unchanged")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cam-width", type=int, default=640)
    ap.add_argument("--cam-height", type=int, default=480)
    ap.add_argument("--cam-fps", type=int, default=30,
                    help="camera capture fps (UVC MJPG)")
    ap.add_argument("--relay-fps", type=int, default=4,
                    help="preview frames per second relayed to MP157 "
                         "(LCD smoothness; independent of --cam-fps)")
    ap.add_argument("--jpeg-quality", type=int, default=70)
    ap.add_argument("--bind", default="192.168.10.2",
                    help="relay listen address; default = the MP157 link "
                         "address only, so the unauthenticated stream is not "
                         "reachable from other interfaces")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--period", type=int, default=DEFAULT_PERIOD_MS,
                    help="recognition period, ms")
    ap.add_argument("--fresh", type=int, default=DEFAULT_FRESH_MS,
                    help="max JPEG age to recognize, ms")
    ap.add_argument("--model", default="/root/models/lprnet.rknn")
    ap.add_argument("--min-conf", type=float, default=0.70)
    ap.add_argument("--min-chars", type=int, default=5,
                    help="plates shorter than this are treated as no_plate "
                         "(Chinese plates are 7-8 chars)")
    ap.add_argument("--roi", default="0,0,1,1",
                    help="plate crop region as x,y,w,h in RELATIVE 0..1 "
                         "coordinates, applied before the 94x24 resize. "
                         "Default = full frame. Ignored when --det-model is "
                         "set (detection finds the plate). Tune with the relay "
                         "frame grab (see README).")
    ap.add_argument("--det-model", default="",
                    help="path to a yolov8 plate-detection .rknn; when set, "
                         "recognition becomes two-stage (detect -> crop -> "
                         "LPRNet) and --roi is ignored")
    ap.add_argument("--det-conf", type=float, default=0.25,
                    help="detection confidence threshold (two-stage only)")
    ap.add_argument("--det-margin", type=float, default=0.1,
                    help="crop margin around the detected box, fraction of "
                         "box size (two-stage only)")
    args = ap.parse_args(argv)

    roi = None
    try:
        parts = [float(v) for v in args.roi.split(",")]
        if len(parts) == 4:
            roi = tuple(parts)
    except ValueError:
        roi = None

    hub = LineHub()
    latest = {}

    recognizer = None
    try:
        if args.det_model:
            recognizer = TwoStageRecognizer(
                args.det_model, args.model, margin=args.det_margin,
                conf_thres=args.det_conf)
            print("edge hub: two-stage models loaded: det=%s rec=%s "
                  "margin=%.2f det_conf=%.2f (roi ignored)"
                  % (args.det_model, args.model, args.det_margin,
                     args.det_conf), flush=True)
        else:
            recognizer = Recognizer(args.model, roi=roi)
            print("edge hub: model loaded: %s roi=%s"
                  % (args.model, args.roi), flush=True)
    except Exception as exc:
        print("edge hub: WARNING recognizer unavailable (%r) - "
              "preview relay only" % (exc,), file=sys.stderr, flush=True)

    # Bind with retry: at boot the static address may not be configured yet
    # (networkd applies it on carrier). Refuse 0.0.0.0 as the default bind
    # via the --bind help text; an explicit 0.0.0.0 stays possible for labs.
    srv = None
    for _attempt in range(30):
        try:
            ThreadingTCPServer.allow_reuse_address = True
            srv = ThreadingTCPServer((args.bind, args.port),
                                     make_handler(hub))
            break
        except OSError as exc:
            print("edge hub: cannot bind %s:%d (%s) - retry in 2s"
                  % (args.bind, args.port, exc), flush=True)
            time.sleep(2)
    if srv is None:
        print("edge hub: bind failed, giving up", file=sys.stderr, flush=True)
        sys.exit(1)
    srv.daemon_threads = True

    if os.path.basename(args.source).startswith("video"):
        reader = CameraThread(hub, latest, args.source, args.cam_width,
                              args.cam_height, args.cam_fps,
                              args.relay_fps, args.jpeg_quality, roi=roi)
    else:
        reader = ReaderThread(hub, latest, args.source, args.baud)
    recog = RecogThread(hub, latest, recognizer, args.period, args.fresh,
                        args.min_conf, min_chars=args.min_chars)
    reader.start()
    recog.start()
    print("edge hub: listening on %s:%d source=%s period=%dms"
          % (args.bind, args.port, args.source, args.period), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        reader.stop()
        recog.stop()
        srv.server_close()
        print("edge hub: stopped", flush=True)


if __name__ == "__main__":
    main()

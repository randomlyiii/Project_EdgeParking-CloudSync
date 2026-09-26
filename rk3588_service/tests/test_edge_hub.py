# -*- coding: ascii -*-
"""Host unit tests for edge_hub (no numpy/rknn needed)."""
import base64
import json
import os
import queue
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import edge_hub  # noqa: E402
import lpr_decode  # noqa: E402

JPEG_DUMMY = b"\xff\xd8\xff\xe0" + b"fake-jpeg" * 16 + b"\xff\xd9"


def k210_lines(jpeg, chunk=900):
    b64 = base64.b64encode(jpeg).decode("ascii")
    lines = []
    for off in range(0, len(b64), chunk):
        lines.append("K2:IMG:%d:%s" % (off, b64[off:off + chunk]))
    lines.append("K2:END:%d" % len(b64))
    return lines


def logits_from_plate(plate):
    idx = [lpr_decode.CHARS.index(ch) for ch in plate]
    seq = [lpr_decode.BLANK_INDEX] + idx + [lpr_decode.BLANK_INDEX]
    rows = []
    for cls in seq:
        row = [0.0] * lpr_decode.NUM_CLASSES
        row[cls] = 10.0
        rows.append(row)
    return rows


class StubRecognizer(object):
    def __init__(self, plate="\u7ca4B12345"):
        self._plate = plate

    def logits(self, jpeg):
        return logits_from_plate(self._plate)

    def plate(self, jpeg):
        if not self._plate:
            return "", 0.0, {}
        _p, _c, _s = lpr_decode.ctc_greedy_decode(
            logits_from_plate(self._plate))
        return self._plate, 0.99, {}


class TestLineEmission(unittest.TestCase):
    """jpeg_to_k2_lines must reassemble through FrameAssembler (park_ui
    parses exactly this stream - a local V4L2 camera must be transparent)."""

    def test_roundtrip(self):
        lines = edge_hub.jpeg_to_k2_lines(JPEG_DUMMY, chunk=256)
        self.assertTrue(lines[-1].startswith("K2:END:"))
        asm = edge_hub.FrameAssembler()
        out = None
        for line in lines:
            got = asm.feed_line(line)
            if got is not None:
                out = got
        self.assertEqual(out, JPEG_DUMMY)


class TestFrameAssembler(unittest.TestCase):
    def test_roundtrip(self):
        asm = edge_hub.FrameAssembler()
        out = None
        for line in k210_lines(JPEG_DUMMY):
            got = asm.feed_line(line)
            if got is not None:
                out = got
        self.assertEqual(out, JPEG_DUMMY)

    def test_partial_returns_none(self):
        asm = edge_hub.FrameAssembler()
        lines = k210_lines(JPEG_DUMMY)
        for line in lines[:-1]:
            self.assertIsNone(asm.feed_line(line))

    def test_len_mismatch_dropped(self):
        asm = edge_hub.FrameAssembler()
        for line in k210_lines(JPEG_DUMMY)[:-1]:
            asm.feed_line(line)
        self.assertIsNone(asm.feed_line("K2:END:999999"))

    def test_non_k2_ignored(self):
        asm = edge_hub.FrameAssembler()
        self.assertIsNone(asm.feed_line("[stat] fps=1 jpeg=100B"))
        self.assertIsNone(asm.feed_line(""))
        self.assertIsNone(asm.feed_line("K2:IMG:bad"))
        self.assertIsNone(asm.feed_line("K2:END:xyz"))

    def test_chunk_cap_resync(self):
        asm = edge_hub.FrameAssembler()
        cap = edge_hub.FrameAssembler.MAX_CHUNKS
        for i in range(cap + 5):
            asm.feed_line("K2:IMG:%d:%s" % (i, "A" * 100))
        # the frame right after the flood is corrupted by stale chunks and
        # dropped (length mismatch); the NEXT frame assembles cleanly
        self.assertIsNone(asm.feed_line("K2:END:1"))
        got = None
        for line in k210_lines(JPEG_DUMMY):
            got = asm.feed_line(line)
        self.assertEqual(got, JPEG_DUMMY)


class FakeHub(object):
    def __init__(self):
        self.lines = []
        self._lock = threading.Lock()

    def broadcast(self, line):
        with self._lock:
            self.lines.append(line)


class TestRecogThread(unittest.TestCase):
    def _run(self, plate="\u7ca4B12345", min_conf=0.7, wait_s=0.5):
        hub = FakeHub()
        latest = {"jpeg": JPEG_DUMMY,
                  "at": int(time.time() * 1000)}
        rec = edge_hub.RecogThread(hub, latest, StubRecognizer(plate),
                                   period_ms=50, fresh_ms=10000,
                                   min_conf=min_conf)
        rec.start()
        try:
            deadline = time.time() + wait_s
            while time.time() < deadline and not hub.lines:
                time.sleep(0.01)
        finally:
            rec.stop()
            rec.join(timeout=2)
        return hub.lines

    def test_ok_line_format(self):
        lines = self._run()
        self.assertTrue(lines, "no result line emitted")
        line = lines[0]
        self.assertTrue(line.startswith("K2:OK:"), line)
        obj = json.loads(line[len("K2:OK:"):])
        self.assertEqual(obj["plate"], "\u7ca4B12345")
        self.assertGreater(obj["confidence"], 0.9)

    def test_ok_line_ascii_on_wire(self):
        lines = self._run()
        lines[0].encode("ascii")        # must not raise

    def test_stale_frame_skipped(self):
        hub = FakeHub()
        latest = {"jpeg": JPEG_DUMMY,
                  "at": int(time.time() * 1000) - 60000}
        rec = edge_hub.RecogThread(hub, latest, StubRecognizer(),
                                   period_ms=50, fresh_ms=10000,
                                   min_conf=0.7)
        rec.start()
        time.sleep(0.3)
        rec.stop()
        rec.join(timeout=2)
        self.assertEqual(hub.lines, [])

    def test_low_conf_ng(self):
        lines = self._run(min_conf=0.9999)
        self.assertTrue(lines[0].startswith("K2:NG:low_conf"), lines)

    def test_short_plate_ng(self):
        # a 2-char "plate" at full confidence is background junk (seen live:
        # "jin A" conf 1.000 on a bare wall); must be filtered as no_plate
        lines = self._run(plate="AB")
        self.assertTrue(lines[0].startswith("K2:NG:no_plate"), lines)

    def test_empty_plate_ng(self):
        lines = self._run(plate="")
        self.assertTrue(lines[0].startswith("K2:NG:no_plate"), lines)


class TestLineHubTcp(unittest.TestCase):
    def test_relay_over_tcp(self):
        hub = edge_hub.LineHub(max_queue=8)
        srv = edge_hub.ThreadingTCPServer(("127.0.0.1", 0),
                                          edge_hub.make_handler(hub))
        srv.daemon_threads = True
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            sock = socket.create_connection(srv.server_address, timeout=5)
            sock.settimeout(5)
            try:
                # wait until the server-side handler registered its queue,
                # otherwise the first broadcast races the client setup
                deadline = time.time() + 5
                while hub.client_count() < 1 and time.time() < deadline:
                    time.sleep(0.01)
                self.assertEqual(hub.client_count(), 1)
                f = sock.makefile("rb")
                hub.broadcast("hello")
                hub.broadcast("K2:END:3")
                got = [f.readline().decode("ascii").strip()
                       for _ in range(2)]
                self.assertEqual(got, ["hello", "K2:END:3"])
            finally:
                sock.close()
        finally:
            srv.shutdown()
            srv.server_close()

    def test_slow_client_dropped(self):
        hub = edge_hub.LineHub(max_queue=2)
        q = queue.Queue(maxsize=2)
        hub.add(q)
        hub.broadcast("a")
        hub.broadcast("b")
        hub.broadcast("c")              # queue full -> client dropped
        self.assertEqual(hub.client_count(), 0)


if __name__ == "__main__":
    unittest.main()

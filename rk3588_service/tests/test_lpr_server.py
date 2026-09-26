# -*- coding: ascii -*-
"""Host unit tests for lpr_server with a stub recognizer (no numpy/rknn)."""
import http.client
import json
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import lpr_decode  # noqa: E402
import lpr_server  # noqa: E402

JPEG_DUMMY = b"\xff\xd8\xff\xe0" + b"not-a-real-jpeg" * 8 + b"\xff\xd9"


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
    """Returns a fixed plate, or None for 'undecodable image'."""

    def __init__(self, plate="\u7ca4B12345", fail_image=False):
        self.plate = plate
        self.fail_image = fail_image
        self.calls = 0

    def logits(self, jpeg_bytes):
        self.calls += 1
        if self.fail_image:
            return None
        return logits_from_plate(self.plate)


class TestRoiClamp(unittest.TestCase):
    """The Recognizer ROI normaliser is pure python (no rknn import)."""

    def test_norm_roi_clamps(self):
        n = lpr_server.Recognizer._norm_roi
        self.assertIsNone(n(None))
        self.assertEqual(n((0.1, 0.2, 0.5, 0.5)), (0.1, 0.2, 0.5, 0.5))
        # out-of-range parts get clamped instead of rejected
        x, y, w, h = n((-0.1, 0.9, 2.0, 2.0))
        self.assertGreaterEqual(x, 0.0)
        self.assertGreaterEqual(y, 0.0)
        self.assertLessEqual(x + w, 1.0)
        self.assertLessEqual(y + h, 1.0)
        # degenerate input shape -> None
        self.assertIsNone(n((1.0, 1.0, 0.0, 0.0)))


class ServerCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.recognizer = StubRecognizer()
        cls.srv = lpr_server.make_server("127.0.0.1", 0, cls.recognizer,
                                         "/tmp/model.rknn")
        cls.port = cls.srv.server_address[1]
        cls.thread = threading.Thread(target=cls.srv.serve_forever,
                                      daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def _post(self, path, body, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        hdrs = {"Content-Type": "image/jpeg"}
        if headers:
            hdrs.update(headers)
        conn.request("POST", path, body=body, headers=hdrs)
        resp = conn.getresponse()
        data = resp.read()
        code = resp.status
        conn.close()
        return code, json.loads(data.decode("ascii"))

    def _get(self, path):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        conn.request("GET", path)
        resp = conn.getresponse()
        data = resp.read()
        code = resp.status
        conn.close()
        return code, json.loads(data.decode("ascii"))

    def test_health_ok(self):
        code, obj = self._get("/health")
        self.assertEqual(code, 200)
        self.assertTrue(obj["ok"])
        self.assertEqual(obj["classes"], 68)

    def test_recognize_ok(self):
        code, obj = self._post("/recognize", JPEG_DUMMY)
        self.assertEqual(code, 200)
        self.assertEqual(obj["plate"], "\u7ca4B12345")
        self.assertGreater(obj["conf"], 0.9)
        self.assertIn("ms", obj)

    def test_recognize_response_is_ascii(self):
        _code, obj = self._post("/recognize", JPEG_DUMMY)
        json.dumps(obj, ensure_ascii=True).encode("ascii")  # must not raise

    def test_no_plate(self):
        rec = StubRecognizer(plate="")
        srv = lpr_server.make_server("127.0.0.1", 0, rec, "/tmp/m.rknn")
        port = srv.server_address[1]
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("POST", "/recognize", body=JPEG_DUMMY)
            resp = conn.getresponse()
            obj = json.loads(resp.read().decode("ascii"))
            self.assertEqual(resp.status, 200)
            self.assertEqual(obj["error"], "no_plate")
            conn.close()
        finally:
            srv.shutdown()
            srv.server_close()

    def test_low_conf(self):
        rec = StubRecognizer(plate="\u7ca4B12345")
        srv = lpr_server.make_server("127.0.0.1", 0, rec, "/tmp/m.rknn",
                                     min_conf=0.999)
        port = srv.server_address[1]
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("POST", "/recognize", body=JPEG_DUMMY)
            resp = conn.getresponse()
            obj = json.loads(resp.read().decode("ascii"))
            self.assertEqual(resp.status, 200)
            self.assertEqual(obj["error"], "low_conf")
            conn.close()
        finally:
            srv.shutdown()
            srv.server_close()

    def test_bad_image(self):
        rec = StubRecognizer(fail_image=True)
        srv = lpr_server.make_server("127.0.0.1", 0, rec, "/tmp/m.rknn")
        port = srv.server_address[1]
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("POST", "/recognize", body=JPEG_DUMMY)
            resp = conn.getresponse()
            obj = json.loads(resp.read().decode("ascii"))
            self.assertEqual(resp.status, 400)
            self.assertEqual(obj["error"], "bad_image")
            conn.close()
        finally:
            srv.shutdown()
            srv.server_close()

    def test_bad_length(self):
        code, obj = self._post("/recognize", b"",
                               headers={"Content-Length": "0"})
        self.assertEqual(code, 400)
        self.assertEqual(obj["error"], "bad_length")

    def test_rknn_unavailable(self):
        srv = lpr_server.make_server("127.0.0.1", 0, None, "/tmp/m.rknn")
        port = srv.server_address[1]
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            conn.request("GET", "/health")
            resp = conn.getresponse()
            obj = json.loads(resp.read().decode("ascii"))
            self.assertEqual(resp.status, 200)
            self.assertFalse(obj["ok"])
            conn.request("POST", "/recognize", body=JPEG_DUMMY)
            resp = conn.getresponse()
            obj = json.loads(resp.read().decode("ascii"))
            self.assertEqual(resp.status, 503)
            self.assertEqual(obj["error"], "rknn unavailable")
            conn.close()
        finally:
            srv.shutdown()
            srv.server_close()

    def test_unknown_path_404(self):
        code, _obj = self._get("/nope")
        self.assertEqual(code, 404)
        code, _obj = self._post("/nope", JPEG_DUMMY)
        self.assertEqual(code, 404)


if __name__ == "__main__":
    unittest.main()

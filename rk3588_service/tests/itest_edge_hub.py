# -*- coding: ascii -*-
"""On-board integration test for edge_hub: fake the K210 with a pty, feed a
real plate JPEG as K2: lines, expect the relay to echo them and the recognizer
to inject K2:OK with the right plate.

Runs ON THE RK3588 (needs rknnlite + the converted model). Prereqs:
    /tmp/plate_crop.jpg   a plate crop (see README "smoke")
Then:  python3 tests/itest_edge_hub.py   (exit 0 = PASS)"""
import base64
import json
import os
import socket
import subprocess
import sys
import time

JPEG = open("/tmp/plate_crop.jpg", "rb").read()
B64 = base64.b64encode(JPEG).decode("ascii")
LINES = ["K2:IMG:%d:%s" % (i, B64[i:i + 900]) for i in range(0, len(B64), 900)]
LINES.append("K2:END:%d" % len(B64))


def main():
    master, slave = os.openpty()
    slave_name = os.ttyname(slave)
    proc = subprocess.Popen(
        [sys.executable, "/opt/rk3588_service/edge_hub.py",
         "--tty", slave_name, "--bind", "127.0.0.1", "--port", "8091",
         "--period", "800"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rc = 1
    try:
        client = None
        for _ in range(50):
            try:
                client = socket.create_connection(("127.0.0.1", 8091), timeout=2)
                break
            except OSError:
                time.sleep(0.1)
        if client is None:
            print("FAIL: hub not listening on 8091")
            return 1
        client.settimeout(1)
        buf = b""
        relay_ok = False
        recog_ok = False
        for ln in LINES:
            os.write(master, (ln + "\n").encode("ascii"))
        deadline = time.time() + 20
        while time.time() < deadline and not (relay_ok and recog_ok):
            try:
                chunk = client.recv(65536)
                if not chunk:
                    print("FAIL: relay closed the connection")
                    return 1
                buf += chunk
            except socket.timeout:
                continue
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line.startswith(b"K2:END:"):
                    relay_ok = True
                elif line.startswith(b"K2:OK:"):
                    obj = json.loads(line[len(b"K2:OK:"):].decode("ascii"))
                    plate = obj.get("plate", "")
                    conf = obj.get("confidence", 0)
                    print("recog line: plate=%r conf=%.4f" % (plate, conf))
                    if plate == "\u5dddA88888" and conf > 0.9:
                        recog_ok = True
        if relay_ok and recog_ok:
            print("ITEST PASS: relay + NPU recog + K2:OK injection all OK")
            rc = 0
        else:
            print("ITEST FAIL: relay_ok=%s recog_ok=%s" % (relay_ok, recog_ok))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        os.close(master)
        os.close(slave)
    return rc


if __name__ == "__main__":
    sys.exit(main())

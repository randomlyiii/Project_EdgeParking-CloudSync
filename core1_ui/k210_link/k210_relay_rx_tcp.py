#!/usr/bin/env python3
# -*- coding: ascii -*-
# K210 relay receiver over TCP (edge_hub on the RK3588 -> park_ui link).
# Same K2: line protocol as k210_preview_rx_text.py, transport = TCP socket.
# Usage: python3 k210_relay_rx_tcp.py [rk3588] [8089] [--seconds 10]
#
# Prints what flows on the relay (stats) and saves the newest complete JPEG
# to /tmp/k210_relay_frame.jpg. Pure stdlib, runs on the MP157 board as-is.
import base64
import socket
import sys
import time


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "rk3588"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8089
    seconds = 10
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])

    print("[OK] connect %s:%d, listen %ds (Ctrl-C stop)" % (host, port, seconds))
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(2)

    buf = b""
    chunks = {}
    counts = {"img": 0, "end": 0, "ok": 0, "ng": 0, "stat": 0, "other": 0}
    frame = None
    samples = []
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            data = s.recv(65536)
            if not data:
                print("[DOWN] relay closed the connection")
                break
            buf += data
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if line.startswith(b"K2:IMG:"):
                counts["img"] += 1
                rest = line[len(b"K2:IMG:"):]
                off, _, b64 = rest.partition(b":")
                try:
                    chunks[int(off)] = b64
                except ValueError:
                    pass
            elif line.startswith(b"K2:END:"):
                counts["end"] += 1
                try:
                    want = int(line[len(b"K2:END:"):])
                except ValueError:
                    want = 0
                data = b"".join(chunks[k] for k in sorted(chunks))
                chunks = {}
                if want > 0 and len(data) == want:
                    frame = base64.b64decode(data)
            elif line.startswith(b"K2:OK:"):
                counts["ok"] += 1
                if len(samples) < 5:
                    samples.append(line[:100])
            elif line.startswith(b"K2:NG:"):
                counts["ng"] += 1
                if len(samples) < 5:
                    samples.append(line[:100])
            elif b"stat" in line:
                counts["stat"] += 1
                if len(samples) < 5:
                    samples.append(line[:100])
            else:
                counts["other"] += 1
    s.close()
    for x in samples:
        print("SAMPLE", x.decode("ascii", "replace"))
    print("counts:", counts)
    if frame:
        path = "/tmp/k210_relay_frame.jpg"
        with open(path, "wb") as f:
            f.write(frame)
        print("frame: %d bytes -> %s" % (len(frame), path))


if __name__ == "__main__":
    main()

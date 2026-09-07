#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# K210 text-channel receiver (zero-wire, over console/USB).
# Parses lines:  K2:IMG:<offset>:<base64chunk>  ...  K2:END:<base64_len>
# then decodes the full JPEG and saves it.
# Usage: python3 k210_preview_rx_text.py [/dev/ttyACM0] [--baud 115200]
#        (baud must match the K210 console baud; default 115200)
import os
import select
import termios
import argparse
import binascii
import time


def open_serial(dev, baud):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
              termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    bc = getattr(termios, "B%d" % baud, None)
    if bc is None:
        baud = 115200
        bc = termios.B115200
    a[4] = a[5] = bc
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd, baud


ap = argparse.ArgumentParser()
ap.add_argument("dev", nargs="?", default="/dev/ttyACM0")
ap.add_argument("--baud", type=int, default=115200)
ap.add_argument("--out", default="/tmp/k210_frame.jpg")
a = ap.parse_args()

fd, baud = open_serial(a.dev, a.baud)
print("[OK] %s @%d text-recv (Ctrl-C stop)" % (a.dev, baud))

buf = b""
chunks = {}
frames = 0

while True:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        buf += os.read(fd, 4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode("utf-8", "ignore").strip("\r")
            if line.startswith("K2:IMG:"):
                try:
                    rest = line[len("K2:IMG:"):]
                    off, _, b64 = rest.partition(":")
                    chunks[int(off)] = b64
                except Exception:
                    pass
            elif line.startswith("K2:END:"):
                try:
                    want = int(line[len("K2:END:"):])
                    data = "".join(chunks[k] for k in sorted(chunks))
                    if len(data) == want and chunks:
                        raw = binascii.a2b_base64(data.encode())
                        with open(a.out, "wb") as f:
                            f.write(raw)
                        frames += 1
                        print("frame#%d %dB -> %s" % (frames, len(raw), a.out))
                    else:
                        print("[drop] len mismatch want=%d got=%d chunks=%d"
                              % (want, len(data), len(chunks)))
                except Exception as e:
                    print("[drop] parse err", e)
                chunks = {}

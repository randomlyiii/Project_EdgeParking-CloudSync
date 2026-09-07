#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# K210 preview frame receiver (lite, ASCII-only for safe serial paste).
# Usage: python3 k210_preview_rx_lite.py [/dev/ttyACM0] [--baud 921600] [--out /tmp/k210_frame.jpg]
import os
import select
import termios
import argparse


def crc16(d, crc=0):
    for b in d:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


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
ap.add_argument("--baud", type=int, default=921600)
ap.add_argument("--out", default="/tmp/k210_frame.jpg")
a = ap.parse_args()

fd, baud = open_serial(a.dev, a.baud)
print("[OK] %s @%d (Ctrl-C to stop)" % (a.dev, baud), flush=True)

buf = bytearray()
chunks = bytearray()
frames = 0
drops = 0
while True:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        buf += os.read(fd, 4096)
        while True:
            n = len(buf)
            if n < 9:
                break
            if buf[0] != 0xAA or buf[1] != 0x55:
                del buf[0]
                continue
            plen = int.from_bytes(buf[5:7], "little")
            if plen > 1024:
                del buf[0]
                continue
            need = 7 + plen + 2
            if n < need:
                break
            if crc16(bytes(buf[2:7 + plen])) != int.from_bytes(buf[7 + plen:9 + plen], "little"):
                del buf[0]
                continue
            t = buf[2]
            payload = bytes(buf[7:7 + plen])
            del buf[:need]
            if t == 0x01:
                chunks += payload
            elif t == 0x02 and len(payload) >= 5:
                total = int.from_bytes(payload[:4], "little")
                if total and len(chunks) == total:
                    frames += 1
                    with open(a.out, "wb") as f:
                        f.write(bytes(chunks))
                    print("frame#%d %dB -> %s (drops=%d)"
                          % (frames, total, a.out, drops), flush=True)
                else:
                    drops += 1
                chunks = bytearray()

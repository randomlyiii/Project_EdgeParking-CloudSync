#!/usr/bin/env python3
"""rpmsg_link_test.py - A7(MP157) <-> M4 RPMSG link check. ASCII only.

Run ON THE BOARD as root, AFTER the M4 firmware is running and
/dev/ttyRPMSG0 exists:

    python3 rpmsg_link_test.py                # query M4 state + listen 3s
    python3 rpmsg_link_test.py --listen=6     # listen 6s
    python3 rpmsg_link_test.py --open         # also pulse gate OPEN  (0x11)
    python3 rpmsg_link_test.py --close        # also pulse gate CLOSE (0x12)

IMPORTANT: do not run this while core0_business / rpmsg_demo is running -
two readers on the same tty split the byte stream and both look broken.

Frame: AA 55 | type | seq(2 LE) | len(2 LE) | payload | CRC16(2 LE over type..payload)
Types: 0x11 open / 0x12 close / 0x13 query -> M4 answers 0x23
       0x21 node semantic event (9B: code|arg LE|status|node_id|tick LE) / 0x22 node offline(0)|online(1)
       0x23 M4 state (gate, node_online, can_err u16 LE) / 0x7E heartbeat (1B seq)

Exit code 0 = all checks passed.
"""
import os
import select
import struct
import sys
import time

DEV = os.environ.get("PARK_DEV", "/dev/ttyRPMSG0")
MAGIC = b"\xaa\x55"
PAYLOAD_MAX = 480

# frame type -> name
NAMES = {
    0x11: "gate-open(cmd)",
    0x12: "gate-close(cmd)",
    0x13: "query-state(cmd)",
    0x21: "node-event",
    0x22: "node-state",
    0x23: "M4-state",
    0x7E: "heartbeat",
}

# node event codes (same table as CAN 0x200 d[0]; docs/protocols.md section 1)
EVT = {
    0x01: "CAR_ARRIVE",
    0x02: "CAR_LEAVE",
    0x03: "NODE_FAULT",
    0x04: "GATE_STATE",
    0x05: "NODE_READY",
}


def crc16(data):
    """CRC-16/XMODEM: poly 0x1021, init 0, no reflection, no final xor."""
    crc = 0
    for b in data:
        crc ^= (b << 8) & 0xFFFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build(ftype, seq, payload=b""):
    body = bytes([ftype]) + struct.pack("<HH", seq, len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


def parse(buf):
    """(frames, leftover); frames = [(type, seq, payload), ...]

    Layout (rpmsg_types.h / rpmsg_proto.c rpmsg_frame_build):
      total = 2 magic + 1 type + 2 seq + 2 len + plen + 2 crc = 9 + plen
      payload at offset 7, crc over buf[2 : 7+plen] (type..payload), LE.
    """
    out = []
    i = 0
    n = len(buf)
    while i < n:
        j = buf.find(MAGIC, i)
        if j < 0:
            return out, b""
        if n - j < 7:                       # header not complete yet
            return out, buf[j:]
        ln = struct.unpack_from("<H", buf, j + 5)[0]
        if ln > PAYLOAD_MAX:
            i = j + 1
            continue
        need = 9 + ln
        if n - j < need:
            return out, buf[j:]
        body = buf[j + 2:j + 7 + ln]
        if crc16(body) == struct.unpack_from("<H", buf, j + 7 + ln)[0]:
            out.append((buf[j + 2],
                        struct.unpack_from("<H", buf, j + 3)[0],
                        buf[j + 7:j + 7 + ln]))
            i = j + need
        else:
            i = j + 1
    return out, b""


def describe(ftype, pl):
    if ftype == 0x23 and len(pl) >= 4:
        gate, node, err = struct.unpack("<BBH", pl[:4])
        return "gate=%s node=%s can_err=%u" % (
            "OPEN" if gate else "CLOSE", "ONLINE" if node else "OFFLINE", err)
    if ftype == 0x22 and len(pl) >= 1:
        return "node %s" % ("ONLINE" if pl[0] else "OFFLINE")
    if ftype == 0x21 and len(pl) == 17:
        # interface v2 changed the 0x21 payload: a 17B frame means the M4
        # firmware is still the old one (v1 raw CAN passthrough).
        return "V1 CAN-passthrough payload (17B) - reflash M4 with interface v2"
    if ftype == 0x21 and len(pl) >= 9:
        code, arg, status, node = struct.unpack_from("<BHBB", pl, 0)
        tick = struct.unpack_from("<I", pl, 5)[0]
        return "node=%u ev=0x%02X(%s) arg=%u status=0x%02X tick=%u" % (
            node, code, EVT.get(code, "?"), arg, status, tick)
    if ftype == 0x7E and len(pl) >= 1:
        return "seq=%u" % pl[0]
    return "len=%u" % len(pl)


def open_raw(path):
    """Linux-only (termios); imported here so the protocol layer stays testable."""
    import termios
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = a
    cflag = (cflag & ~termios.CSIZE) | termios.CS8 | termios.CLOCAL | termios.CREAD
    cflag &= ~(termios.PARENB | termios.CSTOPB)
    a = [0, 0, cflag, 0, ispeed, ospeed, cc]
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def main():
    listen = 3.0
    do_open = "--open" in sys.argv
    do_close = "--close" in sys.argv
    for a in sys.argv[1:]:
        if a.startswith("--listen="):
            listen = float(a.split("=", 1)[1])

    if not os.path.exists(DEV):
        print("FAIL: %s not found. Load M4 first:" % DEV)
        print("      sh /root/core0_service/tools/load_m4.sh start")
        return 2

    fd = open_raw(DEV)
    print("opened %s (raw)" % DEV)

    state = {"rx": b"", "counts": {}, "m4": None}
    seq_tx = {}

    def next_seq(t):
        seq_tx[t] = seq_tx.get(t, 0) + 1
        return seq_tx[t]

    def pump(dur):
        t0 = time.time()
        while time.time() - t0 < dur:
            r, _, _ = select.select([fd], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                continue
            state["rx"] += data
            frames, state["rx"] = parse(state["rx"])
            for (t, seq, pl) in frames:
                state["counts"][t] = state["counts"].get(t, 0) + 1
                if t == 0x23 and state["m4"] is None:
                    state["m4"] = (t, seq, pl)
                print("RX 0x%02X %-16s seq=%-5u %s"
                      % (t, NAMES.get(t, "?"), seq, describe(t, pl)))

    def send(t, payload=b""):
        n = next_seq(t)
        fd_ok = os.write(fd, build(t, n, payload))
        print("TX 0x%02X %-16s seq=%-5u (%d bytes)" % (t, NAMES.get(t, "?"), n, fd_ok))
        time.sleep(0.05)

    # --- 1. drain whatever is already buffered (heartbeats etc.) ---
    print("--- drain 0.5s ---")
    pump(0.5)

    # --- 2. query full state: 0x13 -> M4 must answer 0x23 ---
    print("--- query state (0x13) ---")
    send(0x13)
    pump(1.5)

    # --- 3. optional gate pulses ---
    if do_open:
        print("--- gate OPEN (0x11) ---")
        send(0x11)
        pump(1.5)
    if do_close:
        print("--- gate CLOSE (0x12) ---")
        send(0x12)
        pump(1.5)

    # --- 4. listen for heartbeats ---
    print("--- listen %.1fs ---" % listen)
    pump(listen)

    # --- summary ---
    c = state["counts"]
    print("--- summary ---")
    for t in sorted(c):
        print("  0x%02X %-16s x%d" % (t, NAMES.get(t, "?"), c[t]))

    ok_state = c.get(0x23, 0) >= 1
    hb_expect = max(1, int(listen * 2 * 0.6))   # 0x7E every 500ms in firmware
    ok_hb = c.get(0x7E, 0) >= hb_expect

    print("check 0x23 answer to 0x13 : %s (%d)"
          % ("PASS" if ok_state else "FAIL", c.get(0x23, 0)))
    if state["m4"] is not None:
        print("     -> %s" % describe(state["m4"][0], state["m4"][2]))
    print("check 0x7E heartbeat >= %-2d : %s (%d)"
          % (hb_expect, "PASS" if ok_hb else "FAIL", c.get(0x7E, 0)))

    os.close(fd)
    if ok_state and ok_hb:
        print("RESULT: LINK OK")
        return 0
    print("RESULT: LINK PROBLEM (see above)")
    return 1


if __name__ == "__main__":
    sys.exit(main())

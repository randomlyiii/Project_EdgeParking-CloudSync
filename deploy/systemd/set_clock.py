#!/usr/bin/env python3
"""set_clock.py - set the system clock on a board without an RTC.

This board boots at its factory time (2020-02-07, observed 2026-09-11), and a
wrong clock breaks every HTTPS request: certificates from the present are
"not yet valid" for a 2020 system, so the TLS handshake fails and the whole
step-7 cloud fallback looks broken while the network itself is fine.

No NTP client was verified on the image, but HTTP works without TLS, so the
simplest reliable source is the `Date:` header of any reachable HTTP server.
Order is: park-clock.conf list -> this default list.

Usage (root, on the board):
    python3 /opt/park_ui/set_clock.py            # try the default hosts
    python3 /opt/park_ui/set_clock.py -v         # print what it does
    python3 /opt/park_ui/set_clock.py --retries 8 --delay 10
    python3 /opt/park_ui/set_clock.py host:port  # use a specific endpoint

Why the retry loop (2026-09-11 real incident): at boot this unit runs right
after wifi-up. If DHCP/DNS is not ready yet the single attempt failed silently,
the clock stayed at 2020 and every HTTPS request then died with
"CERTIFICATE_VERIFY_FAILED: certificate is not yet valid" - which looks like a
cloud bug, not a clock bug. The unit now passes --retries/--delay.

Exit codes: 0 clock set (or already within tolerance), 1 nothing reachable.
ASCII only (board rule).
"""
import email.utils
import socket
import subprocess
import sys
import time

# Reachable over plain HTTP (no TLS, so a wrong clock cannot break it).
DEFAULT_SOURCES = [
    ("www.baidu.com", 80),
    ("www.aliyun.com", 80),
    ("www.example.com", 80),
]
TOLERANCE_S = 300      # skip the write when the clock is already close enough
TIMEOUT_S = 5


def log(msg, verbose):
    if verbose:
        sys.stderr.write("set_clock: %s\n" % msg)


def http_date(host, port):
    """Return the epoch seconds from an HTTP Date header, or None."""
    req = ("HEAD / HTTP/1.0\r\nHost: %s\r\nUser-Agent: park-clock\r\n"
           "Connection: close\r\n\r\n" % host)
    s = socket.create_connection((host, port), TIMEOUT_S)
    try:
        s.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf and len(buf) < 8192:
            chunk = s.recv(1024)
            if not chunk:
                break
            buf += chunk
    finally:
        s.close()
    for line in buf.decode("latin-1").split("\r\n"):
        if line.lower().startswith("date:"):
            t = email.utils.parsedate_to_datetime(line[5:].strip())
            if t is not None:
                return int(t.timestamp())
    return None


def set_clock(epoch):
    """Set UTC via `date -u -s "YYYY-MM-DD HH:MM:SS"`.

    busybox date does not understand GNU's "@<epoch>" form on every build, so
    pass the explicit string it documents; -u keeps it unambiguous (the HTTP
    Date header is GMT)."""
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(epoch))
    subprocess.check_call(["/bin/sh", "-c", 'date -u -s "%s"' % stamp])


def main(argv):
    verbose = "-v" in argv or "--verbose" in argv
    retries = 1
    delay = 10
    args = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("-v", "--verbose"):
            i += 1
            continue
        if a in ("--retries", "--delay") and i + 1 < len(argv):
            try:
                if a == "--retries":
                    retries = max(1, int(argv[i + 1]))
                else:
                    delay = max(1, int(argv[i + 1]))
            except ValueError:
                pass
            i += 2
            continue
        args.append(a)
        i += 1

    sources = DEFAULT_SOURCES
    if args:
        host, _, port = args[0].partition(":")
        sources = [(host, int(port or 80))]

    attempt = 1
    while attempt <= retries:
        for host, port in sources:
            try:
                epoch = http_date(host, port)
            except Exception as exc:                  # noqa: BLE001
                log("%s:%d failed (%s)" % (host, port, exc), verbose)
                continue
            if epoch is None:
                log("%s:%d has no Date header" % (host, port), verbose)
                continue
            now = int(time.time())
            log("%s:%d says %d (local %d, delta %+d s)"
                % (host, port, epoch, now, epoch - now), verbose)
            if abs(epoch - now) <= TOLERANCE_S:
                print("set_clock: clock already within %d s (%s UTC), nothing "
                      "to do" % (TOLERANCE_S,
                                 time.strftime("%Y-%m-%d %H:%M:%S",
                                               time.gmtime(now))))
                return 0
            set_clock(epoch)
            print("set_clock: clock set to %s UTC from %s:%d (was off by %+d s)"
                  % (time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(epoch)),
                     host, port, epoch - now))
            return 0
        if attempt < retries:
            log("attempt %d/%d found no time source, retrying in %d s"
                % (attempt, retries, delay), True)
            time.sleep(delay)
        attempt += 1

    sys.stderr.write(
        "set_clock: FAILED after %d attempt(s); clock is still %s UTC\n"
        "set_clock: fix by hand (or check wifi-up): "
        "date -u -s \"YYYY-MM-DD HH:MM:SS\"\n"
        % (retries, time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

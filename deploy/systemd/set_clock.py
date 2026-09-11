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
    python3 /opt/park_ui/set_clock.py host:port  # use a specific endpoint

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
    args = [a for a in argv[1:] if not a.startswith("-")]
    sources = DEFAULT_SOURCES
    if args:
        host, _, port = args[0].partition(":")
        sources = [(host, int(port or 80))]

    for host, port in sources:
        try:
            epoch = http_date(host, port)
        except Exception as exc:                      # noqa: BLE001
            log("%s:%d failed (%s)" % (host, port, exc), verbose)
            continue
        if epoch is None:
            log("%s:%d has no Date header" % (host, port), verbose)
            continue
        now = int(time.time())
        log("%s:%d says %d (local %d, delta %+d s)"
            % (host, port, epoch, now, epoch - now), verbose)
        if abs(epoch - now) <= TOLERANCE_S:
            log("clock already within %d s, nothing to do" % TOLERANCE_S,
                verbose)
            return 0
        set_clock(epoch)
        log("clock set to %s" % time.strftime("%Y-%m-%d %H:%M:%S",
                                              time.gmtime(epoch)), True)
        return 0

    log("no usable time source (offline?)", True)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

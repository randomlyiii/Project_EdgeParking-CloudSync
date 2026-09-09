#!/usr/bin/env python3
# Static sanity checks for park_ui Qt sources.
# Runs without a Qt toolchain: verifies pure-ASCII sources (board rule) and
# that the recogResult (3-arg source) / busyChanged signal signatures are wired
# consistently across main.cpp / k210_link.* / mainwindow.*.
import pathlib
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
FILES = ["main.cpp", "mainwindow.h", "mainwindow.cpp", "k210_link.h", "k210_link.cpp"]

ok = True

for f in FILES:
    p = SRC / f
    if not p.exists():
        print("[FAIL] missing", p)
        ok = False
        continue
    b = p.read_bytes()
    bad = [i for i, x in enumerate(b) if x > 127]
    if bad:
        print("[FAIL] %s : non-ASCII bytes at offsets %s" % (f, bad[:8]))
        ok = False
    else:
        print("[pass] %-16s pure ASCII" % f)

# signature consistency: whole-file substring matching (single-line sigs)
checks = [
    ("k210_link.h", "double confidence, int source"),
    ("k210_link.cpp", "double confidence, int source"),
    ("mainwindow.h", "double confidence, int source"),
    ("mainwindow.cpp", "showPlatePopup(plate, confidence, source, false)"),
    ("k210_link.h", "busyChanged(bool busy)"),
    ("k210_link.cpp", "busyChanged(bool busy)"),
    ("mainwindow.h", "onK210Busy(bool busy)"),
    ("main.cpp", "&K210Link::busyChanged"),
    ("main.cpp", "&MainWindow::onK210Busy"),
]
for fn, sub in checks:
    text = (SRC / fn).read_text()
    hit = sub in text
    print("[%s] %-14s contains %-40s (%s)" %
          ("pass" if hit else "FAIL", fn, sub[:40], "ok" if hit else "NO"))
    if not hit:
        ok = False

print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
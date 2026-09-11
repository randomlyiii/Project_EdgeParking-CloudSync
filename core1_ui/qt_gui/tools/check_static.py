#!/usr/bin/env python3
# Static sanity checks for park_ui Qt sources.
# Runs without a Qt toolchain: verifies pure-ASCII sources (board rule) and
# that the recogResult (3-arg source) / busyChanged signal signatures are wired
# consistently across main.cpp / k210_link.* / mainwindow.*.
import pathlib
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"
FILES = ["main.cpp", "mainwindow.h", "mainwindow.cpp", "k210_link.h",
         "k210_link.cpp", "ipc_reader.h", "ipc_reader.cpp",
         "ipc_writer.h", "ipc_writer.cpp"]

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
    # ---- Core1 business write-end (IpcWriter) ----
    ("ipc_writer.h", "class IpcWriter : public QObject"),
    ("ipc_writer.h", "int source"),
    ("ipc_writer.h", "cloudPendingChanged(bool pending)"),
    ("ipc_writer.h", "snapshotRefreshRequested()"),
    ("ipc_writer.cpp", "O_RDWR"),
    ("ipc_writer.cpp", "hb_core1"),
    ("ipc_writer.cpp", "result_valid = 1"),
    ("ipc_writer.cpp", "req_gate_open = 1"),
    ("ipc_writer.cpp", "evt_bits_c1"),
    ("ipc_writer.cpp", "evt_bits_c0"),
    ("ipc_writer.cpp", "__sync_synchronize"),
    ("ipc_writer.cpp", "__sync_add_and_fetch"),
    ("mainwindow.h", "gateRequested(bool open)"),
    ("mainwindow.h", "onCloudPending(bool pending)"),
    ("mainwindow.h", "keyPressEvent(QKeyEvent *e)"),
    ("mainwindow.h", "QPushButton *m_btnGateOpen"),
    ("mainwindow.cpp", "&QPushButton::clicked"),
    ("mainwindow.cpp", "emit gateRequested(true)"),
    ("mainwindow.cpp", "TXT_CLOUDBUSY"),
    ("ipc_reader.h", "bool cloudPending = false"),
    ("ipc_reader.cpp", "cloudPending != m_snap.cloudPending"),
    ("ipc_writer.h", "m_noShmLogged"),
    ("ipc_writer.cpp", "m_noShmLogged = true"),
    ("ipc_writer.cpp", "m_badVerLogged"),
    ("ipc_writer.cpp", "cloud result"),
    ("main.cpp", "&IpcWriter::onRecogResult"),
    ("main.cpp", "&IpcWriter::onRecogFailed"),
    ("main.cpp", "&IpcWriter::snapshotRefreshRequested"),
    ("main.cpp", "&IpcReader::onTick"),
    ("main.cpp", "writer.start()"),
]
for fn, sub in checks:
    text = (SRC / fn).read_text()
    hit = sub in text
    print("[%s] %-14s contains %-40s (%s)" %
          ("pass" if hit else "FAIL", fn, sub[:40], "ok" if hit else "NO"))
    if not hit:
        ok = False

# ---------------------------------------------------------------------------
# Protocol-safety negative checks (core1_business spec 4.3.1 / 5.1.1.3):
# the write-end may only write Core1-owned fields. It must never write the
# Core0-owned fields, never bump seq, and may only CLEAR bits in evt_bits_c0
# (consumer-clear), never set them.
writer = (SRC / "ipc_writer.cpp").read_text()
FORBIDDEN = [
    "->magic =", "->version =", "->seq =", "->seq++", "->seq +=",
    "->free_slots =", "->used_slots =", "->gate_state =", "->link_flags =",
    "->recog_pending =", "->fault_bits =", "->conf_threshold =",
    "->evt_bits_c0 =", "->evt_bits_c0 |=", "->evt_seq_c0 =",
    "->evt_seq_c0++", "->evt_seq_c0 +=",
]
for pat in FORBIDDEN:
    if pat in writer:
        print("[FAIL] ipc_writer.cpp writes Core0-owned field: %s" % pat)
        ok = False
    else:
        print("[pass] ipc_writer.cpp never writes %-24s" % pat)

# and the consumer-clear must be an AND-NOT (clear), not a store
if "__sync_fetch_and_and(&m_shm->evt_bits_c0" in writer:
    print("[pass] ipc_writer.cpp evt_bits_c0 uses consumer-clear (AND-NOT)")
else:
    print("[FAIL] ipc_writer.cpp evt_bits_c0 consumer-clear missing")
    ok = False

print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
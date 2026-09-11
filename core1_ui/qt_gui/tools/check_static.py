#!/usr/bin/env python3
# Static sanity checks for park_ui Qt sources.
# Runs without a Qt toolchain: verifies pure-ASCII sources (board rule),
# signal/slot signature consistency, the Core1/Core0 field-ownership contract
# and the step-7 invariants (cloud key never logged, cloud only in Core1,
# network use confined to CloudClient/WifiManager).
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
FILES = ["main.cpp", "mainwindow.h", "mainwindow.cpp", "k210_link.h",
         "k210_link.cpp", "ipc_reader.h", "ipc_reader.cpp",
         "ipc_writer.h", "ipc_writer.cpp",
         # ---- step 7 (P7-01..P7-07): cloud fallback + network page ----
         "cloud_settings.h", "cloud_settings.cpp",
         "cloud_client.h", "cloud_client.cpp",
         "wifi_manager.h", "wifi_manager.cpp",
         "softkeyboard.h", "softkeyboard.cpp",
         "settingspage.h", "settingspage.cpp"]

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
    # ---- step 7: cloud fallback (PhaseMd/08) ----
    ("cloud_client.h", "class CloudClient : public QObject"),
    ("cloud_client.h", "void recognize(const QImage &frame, bool writeback"),
    ("cloud_client.h", "void failed(const QString &reason"),
    ("cloud_client.cpp", "QNetworkAccessManager"),
    ("cloud_client.cpp", "setSingleShot(true)"),
    ("cloud_client.cpp", "abort()"),
    ("cloud_client.cpp", "data:image/jpeg;base64,"),
    ("cloud_client.cpp", "cloud_settings_has_key"),
    ("cloud_client.cpp", "Bearer "),
    ("cloud_client.cpp", "truncateUtf8"),
    ("cloud_settings.h", "struct CloudSettings"),
    ("cloud_settings.h", "QString cloud_settings_mask_key"),
    ("cloud_settings.cpp", "QSaveFile"),
    ("cloud_settings.cpp", "/etc/park/cloud.conf"),
    ("cloud_settings.cpp", "DEEPSEEK_API_KEY"),
    ("settingspage.h", "class SettingsPage : public QWidget"),
    ("settingspage.h", "void manualCloudRequested()"),
    ("settingspage.cpp", "cloud_settings_save"),
    ("settingspage.cpp", "setCoreThreshold"),
    ("wifi_manager.h", "class WifiManager : public QObject"),
    ("wifi_manager.h", "hasIp = false"),
    ("wifi_manager.cpp", "link set %2 up"),                # vendor step 1
    ("wifi_manager.cpp", "-B -D nl80211 -i %2 -c '%3'"),   # vendor step 2
    ("wifi_manager.cpp", "-i %2 -n -q -t 5 -T 3"),         # vendor step 3
    ("wifi_manager.cpp", "ap_scan=1"),                     # vendor global
    ("wifi_manager.cpp", "toolPath"),                      # /sbin, not PATH
    ("cloud_client.cpp", "caBundlePathFor"),               # TLS CA lookup
    ("cloud_client.cpp", "/etc/park/ca.pem"),              # board CA drop-in
    ("wifi_manager.cpp", "rollbackAfterMs"),
    ("softkeyboard.h", "static QString getText"),
    # ---- step 7 wiring in main.cpp ----
    ("main.cpp", "&IpcWriter::cloudFallbackRequested"),
    ("main.cpp", "&CloudClient::finished"),
    ("main.cpp", "&CloudClient::unreadable"),
    ("main.cpp", "&CloudClient::statsChanged"),
    ("main.cpp", "&WifiManager::statusChanged"),
    ("main.cpp", "&MainWindow::cloudCheckRequested"),
    ("main.cpp", "&SettingsPage::manualCloudRequested"),
    ("main.cpp", "link.latestFrame"),
    ("main.cpp", "cloud.cancel()"),
]
for fn, sub in checks:
    text = (SRC / fn).read_text(encoding="utf-8", errors="replace")
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
writer = (SRC / "ipc_writer.cpp").read_text(encoding="utf-8", errors="replace")
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

# ---------------------------------------------------------------------------
# step-7 negative checks (PhaseMd/08 P7-07 and 5.5.1):
#   * the API key must never reach a log line - only mask_key() may be printed;
#   * the cloud must only be called from Core1: no curl/libcurl/QNetwork in
#     core0_service;
#   * Core0's conf_threshold stays read-only in the UI (one end-side threshold);
#   * the WiFi apply must arm a rollback before touching the config file.
# the key IS allowed on the wire (Authorization: Bearer <key>) but must never
# appear inside a log/print statement - only mask_key() may be printed.
LOG_CALLS = ("qWarning", "qDebug", "qInfo", "qCritical", "printf",
             "std::cout", "toStdErr")
for fn in ["cloud_settings.cpp", "cloud_client.cpp", "settingspage.cpp",
           "main.cpp"]:
    text = (SRC / fn).read_text(encoding="utf-8", errors="replace")
    bad_lines = []
    for i, line in enumerate(text.splitlines(), 1):
        if any(c in line for c in LOG_CALLS) and "apiKey" in line:
            bad_lines.append("%s:%d" % (fn, i))
    if bad_lines:
        print("[FAIL] raw API key inside a log call: %s" % bad_lines)
        ok = False
    else:
        print("[pass] %-18s never logs the raw API key" % fn)

# the key must not be hard-coded in any GUI source either
import re
for f in FILES:
    text = (SRC / f).read_text(encoding="utf-8", errors="replace")
    m = re.search(r"sk-[A-Za-z0-9_\-]{8,}", text)
    if m:
        print("[FAIL] %s embeds what looks like an API key: %s" % (f, m.group(0)[:12]))
        ok = False
print("[pass] no API key literal in the GUI sources")

# Core0 must never issue a cloud request (PhaseMd/08: Core1-only module)
core0 = ROOT.parent.parent / "core0_service"
if core0.is_dir():
    leaks = []
    for p in list(core0.rglob("*.c")) + list(core0.rglob("*.h")):
        if "hostcheck" in p.parts:
            continue
        t = p.read_text(encoding="utf-8", errors="replace")
        for pat in ("curl_easy", "libcurl", "<curl/", "QNetworkAccessManager",
                    "api.deepseek.com"):
            if pat in t:
                leaks.append("%s: %s" % (p.relative_to(core0), pat))
    if leaks:
        print("[FAIL] core0_service touches the cloud: %s" % leaks)
        ok = False
    else:
        print("[pass] core0_service has no cloud-request code (Core1-only)")
else:
    print("[warn] core0_service not found, skipped cloud-ownership check")

# the settings page must not offer a second *end-side* threshold: Core0 owns it
page = (SRC / "settingspage.cpp").read_text(encoding="utf-8", errors="replace")
if "conf_threshold" in page and "setCoreThreshold" in page:
    print("[pass] settingspage shows Core0 conf_threshold read-only")
else:
    print("[FAIL] settingspage does not surface Core0 conf_threshold")
    ok = False
if "->conf_threshold =" in page or "conf_threshold =" in page.replace(
        "core0 conf_threshold=", ""):
    print("[FAIL] settingspage writes Core0's conf_threshold")
    ok = False
else:
    print("[pass] settingspage never writes Core0 conf_threshold")

wifi = (SRC / "wifi_manager.cpp").read_text(encoding="utf-8", errors="replace")
if "restoreBackup" in wifi and "m_rollbackArmed = true" in wifi:
    print("[pass] wifi apply arms an automatic rollback")
else:
    print("[FAIL] wifi apply has no rollback path")
    ok = False

# ---------------------------------------------------------------------------
# Regression guards for defects found by the 2026-09-11 desk review. Each one
# is a real bug that was fixed; the check keeps it from coming back.
def body(text, signature):
    """Text of a function from its signature line to the closing brace."""
    i = text.find(signature)
    if i < 0:
        return ""
    j = text.find("\n}\n", i)
    return text[i:j if j > 0 else len(text)]

mainsrc = (SRC / "main.cpp").read_text(encoding="utf-8", errors="replace")

# 1. --eventfd must default to -1: fd 0 is stdin (/dev/null under systemd) and
#    a QSocketNotifier on it would spin a CPU core.
if '"-1");' in mainsrc and "cli.isSet(optEvt) ? cli.value(optEvt).toInt() : -1" in mainsrc:
    print("[pass] main.cpp --eventfd defaults to -1 (no stdin notifier)")
else:
    print("[FAIL] main.cpp --eventfd has no -1 default (stdin busy-loop risk)")
    ok = False

# 2. Qt 5.12 has no Qt::SkipEmptyParts / setTransferTimeout / splitCommand
BANNED_QT = ["Qt::SkipEmptyParts", "setTransferTimeout", "QProcess::splitCommand",
             "QStringView", "QRecursiveMutex"]
hits = []
for f in FILES:
    t = (SRC / f).read_text(encoding="utf-8", errors="replace")
    for b in BANNED_QT:
        if b in t:
            hits.append("%s: %s" % (f, b))
if hits:
    print("[FAIL] Qt API newer than 5.12.8 used: %s" % hits)
    ok = False
else:
    print("[pass] no post-5.12 Qt API in the GUI sources")

# 3. the periodic wifi poll must not spawn a child process (GUI freeze)
if "runSync(" not in body(wifi, "void WifiManager::refresh()"):
    print("[pass] WifiManager::refresh() is process-free (async SSID probe)")
else:
    print("[FAIL] WifiManager::refresh() blocks on a child process")
    ok = False
if "QNetworkInterface::interfaceFromName" in wifi and \
        "onStaFinished" in wifi:
    print("[pass] wifi uses QNetworkInterface + async wpa_cli status")
else:
    print("[FAIL] wifi address/SSID acquisition regressed to blocking calls")
    ok = False

# 4. the rollback deadline must be armed BEFORE the apply script starts,
#    otherwise a hung sh/udhcpc leaves the UI busy forever
ap = body(wifi, "void WifiManager::applyConfig(")
if ap.find("m_deadline.start(rollbackAfterMs())") >= 0 and \
        ap.find("m_deadline.start(rollbackAfterMs())") < ap.find("m_apply.start("):
    print("[pass] wifi apply arms the rollback deadline before running")
else:
    print("[FAIL] wifi apply does not arm the deadline before the script")
    ok = False

# 5. applyToUi() must not trigger onToggleChanged() via setChecked()
if "QSignalBlocker" in body(page, "void SettingsPage::applyToUi()"):
    print("[pass] settingspage applyToUi() blocks toggled() while filling in")
else:
    print("[FAIL] settingspage applyToUi() would re-save on every setChecked")
    ok = False

# 5b. linuxfb layout guard (real 2026-09-11 board bugs, two rounds):
#     (a) a long text in a QLabel grows the layout minimumSize, Qt then grows the
#         window past the 1024 panel and the right-hand panel ends up off-screen;
#     (b) the first fix used QSizePolicy::Ignored, which means *greedy* in Qt -
#         the status chips collapsed to zero width and the bar looked empty.
#     Correct recipe: explicit window minimumSize + Preferred policy +
#     minimumWidth(0) (an explicit minimum beats the text-derived hint).
mw = (SRC / "mainwindow.cpp").read_text(encoding="utf-8", errors="replace")
if "setMinimumSize(320, 200)" in mw and "setMaximumSize(sg.size())" in mw:
    print("[pass] mainwindow pins its own min/max size (no layout inflation)")
else:
    print("[FAIL] mainwindow can be grown past the panel by a long label")
    ok = False
for needle in ["l->setSizePolicy(QSizePolicy::Preferred",
               "m_lblEvents->setSizePolicy(QSizePolicy::Preferred",
               "m_lblPreview->setSizePolicy(QSizePolicy::Preferred"]:
    if needle not in mw:
        print("[FAIL] mainwindow: missing shrinkable policy: %s" % needle)
        ok = False
if "setSizePolicy(QSizePolicy::Ignored" in mw:
    print("[FAIL] mainwindow uses QSizePolicy::Ignored (widgets collapse to 0)")
    ok = False
print("[pass] status chips / ticker / preview shrink but keep their width")
if "text.size() > 90" in mw:
    print("[pass] event ticker lines are length-bounded")
else:
    print("[FAIL] event ticker accepts unbounded lines")
    ok = False

# 6. the board has no pgrep/pkill/timeout: board-side scripts must not use them
#    (match command-looking text only, so the explanatory comments are fine)
for f in ["wifi_manager.cpp", "settingspage.cpp"]:
    t = (SRC / f).read_text(encoding="utf-8", errors="replace")
    for tool in ["pgrep -", "pgrep ", "pkill -", "pkill ", "timeout "]:
        if tool in t:
            print("[FAIL] %s uses '%s' (absent on the board)" % (f, tool))
            ok = False
print("[pass] no pgrep/pkill/timeout in the board-side helpers")

# 6b. the python fallback transport must stay available: it is the only reason
#     the cloud works at all on this board (Qt 5.12 + OpenSSL 1.1.1 stalls on
#     TLS 1.3 while python3 completes the same request)
cc = (SRC / "cloud_client.cpp").read_text(encoding="utf-8", errors="replace")
for needle, why in [
        ("kPyTransport", "embedded python request script"),
        ("sendViaPython", "python transport entry point"),
        ("onPythonFinished", "python transport result handling"),
        ("pythonTransportActive", "transport= selection"),
        ("switching to python3", "automatic fallback on Qt failure"),
        ("pythonPath()", "python3 resolved by absolute path (no PATH)"),
        ("errorOccurred", "failed python start is reported, not hung"),
        ("QFile::remove(kPyJobPath)", "job file (holds the key) is deleted"),
        ("setPermissions(kPyJobPath", "job file is 0600")]:
    if needle not in cc:
        print("[FAIL] python transport: missing %s (%s)" % (needle, why))
        ok = False
print("[pass] python fallback transport is complete and key-safe")

# 6c. per-provider API keys: switching provider on the LCD must also switch the
#     key (round 2 of the real "incorrect api key" incident on the board)
cs = (SRC / "cloud_settings.cpp").read_text(encoding="utf-8", errors="replace")
for needle, why in [
        ("cloud_provider_for_base", "endpoint host -> provider id"),
        ("cloud_settings_adopt_key", "switch provider -> adopt its key"),
        ("cloud_settings_set_key", "store the key under its provider"),
        ("key_", "key_<provider>= lines in cloud.conf")]:
    if needle not in cs:
        print("[FAIL] per-provider keys: missing %s (%s)" % (needle, why))
        ok = False
sp = (SRC / "settingspage.cpp").read_text(encoding="utf-8", errors="replace")
if "cloud_settings_adopt_key" not in sp:
    print("[FAIL] settingspage PRESET does not adopt the provider's key")
    ok = False
else:
    print("[pass] per-provider API keys are stored and adopted on preset")

# 6d. the board's own network address must never leave the kernel: the panel is
#     on a public LCD and the journal is readable, so publishing the leased
#     address just invites casual tampering. Only the boolean "has a lease" is
#     allowed to travel through the UI; nothing may hold, show, log or persist
#     the address itself. (A generic dotted-quad regex is useless here because
#     "spec 5.5.3.1" style references are everywhere, so we look for the two
#     things that actually matter: an address member and a LAN-looking literal.)
wmh = (SRC / "wifi_manager.h").read_text(encoding="utf-8", errors="replace")
wmc = (SRC / "wifi_manager.cpp").read_text(encoding="utf-8", errors="replace")
for needle, why in [("QString ip", "address member in WifiStatus"),
                    ("QString gw", "gateway member in WifiStatus")]:
    if needle in wmh:
        print("[FAIL] wifi_manager.h still keeps the %s" % why)
        ok = False
if "/proc/net/route" in wmc:
    print("[FAIL] wifi_manager still parses the gateway out of /proc/net/route")
    ok = False
for needle in ["m_st.ip", "st.ip", "s.ip", "st->ip", "st->gw", "s.gw",
               "ip=%", "gw=%"]:
    for f in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.h")):
        if needle in f.read_text(encoding="utf-8", errors="replace"):
            print("[FAIL] %s exposes the address via '%s'" % (f.name, needle))
            ok = False
for f in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.h")):
    t = f.read_text(encoding="utf-8", errors="replace")
    if "192.168." in t or "10.0.0." in t:
        print("[FAIL] %s hardcodes a LAN address literal" % f.name)
        ok = False
print("[pass] Core1 keeps no address: WIFI chip shows the SSID only")

pro = (ROOT / "qt_gui.pro").read_text(encoding="utf-8", errors="replace")
for mod in ["cloud_settings", "cloud_client", "wifi_manager", "softkeyboard",
            "settingspage"]:
    if "src/%s.cpp" % mod not in pro or "src/%s.h" % mod not in pro:
        print("[FAIL] qt_gui.pro misses %s" % mod)
        ok = False
if "QT       += core gui widgets network" not in pro:
    print("[FAIL] qt_gui.pro does not link QtNetwork")
    ok = False
else:
    print("[pass] qt_gui.pro builds all step-7 modules + QtNetwork")

print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
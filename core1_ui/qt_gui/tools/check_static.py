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

# 8. privacy: every real runtime config in this project has a tracked "sample_*"
#    counterpart whose values are placeholders only. The live files (API key,
#    WiFi PSK, board overrides) stay outside the repo and are gitignored; a
#    filled-in sample would leak exactly what those rules protect, so this gate
#    fails on a real-looking value. (2026-09-11: user asked for the sweep.)
import re

REPO = ROOT.parent.parent          # core1_ui/qt_gui -> repository root
SAMPLES = ["deploy/sample_cloud.conf", "deploy/sample_wpa_supplicant.conf",
           "deploy/sample_park-ui.env", "core0_service/sample_core0.conf"]
for extra in sorted(REPO.glob("*/sample_*.txt")):
    SAMPLES.append(str(extra.relative_to(REPO)))
PLACEHOLDERS = {"YOUR_PASSWORD", "YOUR_PSK", "YOUR_SSID", "changeme", ""}

for rel in SAMPLES:
    p = REPO / rel
    if not p.exists():
        print("[FAIL] missing sample %s" % rel)
        ok = False
        continue
    b = p.read_bytes()
    if any(x > 127 for x in b):
        print("[FAIL] %s is not pure ASCII" % rel)
        ok = False
    t = b.decode("utf-8", "replace")
    if re.search(r"sk-[A-Za-z0-9_-]{16,}", t):
        print("[FAIL] %s carries a real-looking API key" % rel)
        ok = False
    for m in re.finditer(r'psk\s*=\s*"([^"]*)"', t):
        if m.group(1) not in PLACEHOLDERS:
            print("[FAIL] %s carries a real-looking WiFi PSK" % rel)
            ok = False
    if re.search(r"\b192\.168\.\d+\.\d+\b", t):
        print("[FAIL] %s hardcodes a LAN address" % rel)
        ok = False
print("[pass] tracked samples exist and hold placeholders only")

# the env sample must not document knobs no source reads (PARK_UI_WIFI was
# documented in the acceptance doc for a while before it was actually wired up)
envt = (REPO / "deploy/sample_park-ui.env").read_text(encoding="utf-8",
                                                      errors="replace")
srcs = "".join(p.read_text(encoding="utf-8", errors="replace")
               for p in sorted(SRC.glob("*.cpp")))
for name in ["PARK_UI_TTY", "PARK_UI_K210_FLIP", "PARK_UI_WIFI",
             "PARK_WPA_CONF", "PARK_CLOUD_CONF"]:
    if name in envt and name not in srcs:
        print("[FAIL] sample_park-ui.env documents %s but no source reads it"
              % name)
        ok = False
print("[pass] every knob in sample_park-ui.env is read by the code")

# .gitignore must keep the live configs out of the index (samples stay tracked)
gi = (REPO / ".gitignore").read_text(encoding="utf-8", errors="replace")
for pat in ["cloud.conf", "core0.conf", "wpa_supplicant.conf", "park-ui.env",
            "key.txt"]:
    if pat not in gi:
        print("[FAIL] .gitignore does not cover %s" % pat)
        ok = False
print("[pass] .gitignore covers every live runtime config")

# 9. boot chain: this board's only link is WiFi and the vendor desktop service
#    that used to raise wlan0 is disabled on purpose - so wifi-up.service must
#    exist, must run the vendor's three steps, must avoid the procps tools this
#    rootfs does not have, and must be ordered before park-clock/park-ui.
#    (2026-09-11 user bug: after every power cycle the three commands had to be
#    typed by hand; a link with no lease then looked like a cloud failure.)
DEPLOY = REPO / "deploy/systemd"
wifi_sh = DEPLOY / "wifi_up.sh"
wifi_svc = DEPLOY / "wifi-up.service"
for p in [wifi_sh, wifi_svc]:
    if not p.exists():
        print("[FAIL] missing %s" % p.name)
        ok = False
if wifi_sh.exists():
    t = wifi_sh.read_text(encoding="utf-8", errors="replace")
    if any(ord(c) > 127 for c in t):
        print("[FAIL] wifi_up.sh is not pure ASCII")
        ok = False
    for needle in ["link set", "-B -D nl80211", "udhcpc", "PARK_UI_WIFI",
                   "PARK_WPA_CONF", "/proc/[0-9]*", "grep -q 'inet '"]:
        if needle not in t:
            print("[FAIL] wifi_up.sh misses '%s'" % needle)
            ok = False
    for tool in ["pgrep ", "pkill ", "timeout "]:
        if tool in t:
            print("[FAIL] wifi_up.sh uses '%s' (absent on the board)" % tool)
            ok = False
    if "psk" in t.lower():
        print("[FAIL] wifi_up.sh mentions the WiFi passphrase")
        ok = False
if wifi_svc.exists():
    t = wifi_svc.read_text(encoding="utf-8", errors="replace")
    for needle in ["Before=park-clock.service", "After=board-power.service",
                   "/opt/park_ui/tools/wifi_up.sh",
                   "EnvironmentFile=-/etc/park-ui.env"]:
        if needle not in t:
            print("[FAIL] wifi-up.service misses '%s'" % needle)
            ok = False
for unit in ["park-ui.service", "park-clock.service"]:
    t = (DEPLOY / unit).read_text(encoding="utf-8", errors="replace")
    if "wifi-up.service" not in t:
        print("[FAIL] %s does not order itself against wifi-up.service" % unit)
        ok = False
ia = (DEPLOY / "install_all.sh").read_text(encoding="utf-8", errors="replace")
for needle in ["wifi_up.sh", "wifi-up.service",
               "systemctl enable wifi-up.service"]:
    if needle not in ia:
        print("[FAIL] install_all.sh misses '%s'" % needle)
        ok = False
print("[pass] WiFi is raised at boot (wifi-up.service + vendor recipe)")

# 9b. a dead link must be reported as "no network", not as a broken transport:
#     the wrong wording sends the next debugging session into the wrong layer.
cct = (SRC / "cloud_client.cpp").read_text(encoding="utf-8", errors="replace")
for needle in ["looksLikeNetworkDown", "no network", "looksLikeCertFailure",
               "certificate verify failed", "verify_message"]:
    if needle not in cct:
        print("[FAIL] cloud_client does not classify a dead link (%s)" % needle)
        ok = False
mjt = (SRC / "main.cpp").read_text(encoding="utf-8", errors="replace")
for needle in ["wifi:lease=", "systemctl restart wifi-up", "/etc/park/ca.pem"]:
    if needle not in mjt:
        print("[FAIL] main.cpp cloud failure has no link-state hint (%s)"
              % needle)
        ok = False
if "wifi:ip=" in mjt:
    print("[FAIL] main.cpp logs the address again")
    ok = False
print("[pass] a dead link reports 'no network' + the wifi-up hint")

# 9c. file locations must not lie: the step-7 cloud code lives in the park_ui
#     process (src/cloud_*.{h,cpp}). Two empty placeholder directories from the
#     original plan used to sit in the tree and made reviewers look in the wrong
#     place (2026-09-11: "core0_service/cloud has no code, I cannot check it").
for dead in [REPO / "core0_service" / "cloud", REPO / "core1_ui" / "cloud_api"]:
    if dead.exists():
        print("[FAIL] dead placeholder dir is back: %s" % dead.name)
        ok = False
cli = (SRC / "cloud_client.cpp").read_text(encoding="utf-8", errors="replace")
cli += (SRC / "cloud_settings.cpp").read_text(encoding="utf-8", errors="replace")
if "QNetworkAccessManager" not in cli:
    print("[FAIL] the cloud transport is not in the park_ui sources")
    ok = False
print("[pass] cloud code lives only in park_ui (no dead placeholder dirs)")

# 9d. the clock is a cloud-critical dependency on this RTC-less board: a single
#     failed sync at boot leaves 2020 and every HTTPS request dies with
#     "certificate is not yet valid" (2026-09-11 real incident, reported as a
#     cloud bug). So: retries inside the helper, a re-sync timer, and an
#     unconditional success line in the journal.
clock_py = (DEPLOY / "set_clock.py").read_text(encoding="utf-8", errors="replace")
try:
    import ast
    ast.parse(clock_py)
except SyntaxError as exc:
    print("[FAIL] set_clock.py does not parse: %s" % exc)
    ok = False
for needle in ["--retries", "--delay", "set_clock: clock set to",
               "date -u -s", "FAILED after"]:
    if needle not in clock_py:
        print("[FAIL] set_clock.py misses '%s'" % needle)
        ok = False
clock_svc = (DEPLOY / "park-clock.service").read_text(encoding="utf-8",
                                                      errors="replace")
for needle in ["--retries 8", "TimeoutStartSec=180", "wifi-up.service"]:
    if needle not in clock_svc:
        print("[FAIL] park-clock.service misses '%s'" % needle)
        ok = False
clock_timer = DEPLOY / "park-clock.timer"
if not clock_timer.exists():
    print("[FAIL] park-clock.timer is missing (no second chance for the clock)")
    ok = False
else:
    t = clock_timer.read_text(encoding="utf-8", errors="replace")
    for needle in ["OnBootSec=3min", "Unit=park-clock.service"]:
        if needle not in t:
            print("[FAIL] park-clock.timer misses '%s'" % needle)
            ok = False
if "park-clock.timer" not in ia:
    print("[FAIL] install_all.sh does not install/enable park-clock.timer")
    ok = False
print("[pass] clock sync retries + re-sync timer + journal success line")

# 9e. an HTTP 200 with an unparseable body must say WHAT came back: "body 795B"
#     alone cannot tell a wrong model from a truncated/filtered answer
#     (2026-09-11 real incident: "FAILED empty content: body 795B").
for needle in ["collapseForLog", "empty content", "reasoning_content",
               "finish_reason", "max_tokens\"), 256"]:
    if needle not in cct:
        print("[FAIL] cloud_client does not explain an unreadable body (%s)"
              % needle)
        ok = False
print("[pass] unreadable bodies are reported with a snippet + reason")

# 9f. the network page must be able to run the vendor recipe on demand: the
#     link is otherwise only raised at boot (wifi-up.service) or by CONNECT,
#     so a board installed before that unit existed cannot be recovered from
#     the panel (2026-09-11 user request).
wmh = (SRC / "wifi_manager.h").read_text(encoding="utf-8", errors="replace")
wmc2 = (SRC / "wifi_manager.cpp").read_text(encoding="utf-8", errors="replace")
spt = (SRC / "settingspage.cpp").read_text(encoding="utf-8", errors="replace")
for needle in ["void bringUp();"]:
    if needle not in wmh:
        print("[FAIL] WifiManager has no bringUp() slot")
        ok = False
for needle in ["void WifiManager::bringUp()", "m_bringUp = true",
               "no rollback timer"]:
    if needle not in wmc2:
        print("[FAIL] wifi_manager bringUp is incomplete (%s)" % needle)
        ok = False
if "bringUp()" not in spt or "m_btnNetUp" not in spt:
    print("[FAIL] the network page has no bring-up button")
    ok = False
print("[pass] the network page can run the vendor wifi recipe on demand")

# 9g. the clock must be settable from the panel: a board with no RTC and no
#     network at boot sits in 2020, and then every HTTPS call fails with
#     "certificate is not yet valid" (2026-09-11 real incident). SYNC runs the
#     same helper as park-clock.service, SETTIME writes an operator-typed UTC
#     stamp through date(1) (argv, no shell) after a strict shape check.
for needle in ["refreshClock", "onClockSet", "onClockSync", "m_lblClock",
               "m_btnClockSync", "m_btnClockSet", "NOT SET: cloud TLS will fail",
               "/opt/park_ui/set_clock.py", "clockToolPath"]:
    if needle not in spt:
        print("[FAIL] settingspage cannot set the clock (%s)" % needle)
        ok = False
if "^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$" not in spt:
    print("[FAIL] the typed clock value is not shape-checked")
    ok = False
print("[pass] the diag page shows the UTC clock and can SYNC / SET it")

# 9h. saving the network settings must rewrite /etc/wpa_supplicant.conf in the
#     vendor layout (ctrl_interface / update_config / ap_scan + one network
#     block). CONNECT writes AND applies and rolls back on a failed lease, so a
#     save-only action is needed (2026-09-11 user request).
for needle in ["void saveConfig(const QString &ssid, const QString &psk);"]:
    if needle not in wmh:
        print("[FAIL] WifiManager has no saveConfig() slot")
        ok = False
for needle in ["void WifiManager::saveConfig", "ctrl_interface=/var/run/wpa_supplicant",
               "update_config=1", "ap_scan=1", "network={"]:
    if needle not in wmc2:
        print("[FAIL] the wifi config writer misses '%s'" % needle)
        ok = False
if "saveConfig(" not in spt or "onSaveNetwork" not in spt:
    print("[FAIL] the network page has no save-only button")
    ok = False
print("[pass] network edits can be written to wpa_supplicant.conf (vendor layout)")

print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
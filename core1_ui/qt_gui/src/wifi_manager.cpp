#include "wifi_manager.h"

#include <QAbstractSocket>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

WifiManager::WifiManager(QObject *parent)
    : QObject(parent)
{
    m_st.iface = m_iface;
    m_watchdog.setInterval(2000);
    connect(&m_watchdog, &QTimer::timeout, this, &WifiManager::onWatchdog);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, &WifiManager::onWatchdog);

    m_apply.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_apply, QOverload<int, QProcess::ExitStatus>::of(
                          &QProcess::finished),
            this, &WifiManager::onApplyFinished);
    m_scan.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_scan, QOverload<int, QProcess::ExitStatus>::of(
                         &QProcess::finished),
            this, &WifiManager::onScanFinished);

    /* async SSID probe: never block the GUI thread on wpa_cli */
    connect(&m_sta, QOverload<int, QProcess::ExitStatus>::of(
                        &QProcess::finished),
            this, &WifiManager::onStaFinished);
}

QString WifiManager::confPath()
{
    const QByteArray env = qgetenv("PARK_WPA_CONF");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QStringLiteral("/etc/wpa_supplicant.conf");
}

/* Marker echoed between the two halves of the async wpa_cli probe. */
const char *const WifiManager::kStaSep = "@@@";

QString WifiManager::backupPath()
{
    return confPath() + QStringLiteral(".bak");
}

void WifiManager::setInterface(const QString &iface)
{
    if (iface.isEmpty() || iface == m_iface)
        return;
    m_iface = iface;
    m_st.iface = iface;
}

/* Short, bounded helpers only: anything that can take seconds runs through the
 * asynchronous QProcess members instead, so the UI never freezes. */
QString WifiManager::runSync(const QString &program, const QStringList &args,
                             int timeoutMs, int *exitCode)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(1500)) {
        if (exitCode != nullptr)
            *exitCode = -1;
        return QString();
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        if (exitCode != nullptr)
            *exitCode = -2;
        return QString::fromLocal8Bit(p.readAll());
    }
    if (exitCode != nullptr)
        *exitCode = p.exitCode();
    return QString::fromLocal8Bit(p.readAll());
}

static QString readFirstLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QString s = QString::fromLocal8Bit(f.readLine()).trimmed();
    f.close();
    return s;
}

/* Absolute path of a board tool. The wifi tools live in /sbin on this rootfs
 * (/sbin/iw, /sbin/wpa_cli, /sbin/udhcpc, /sbin/wpa_supplicant) and a systemd
 * unit's PATH does not necessarily contain /sbin, so never rely on PATH. */
QString WifiManager::toolPath(const QString &name)
{
    static const char *const dirs[] = { "/sbin/", "/usr/sbin/", "/bin/",
                                        "/usr/bin/" };
    if (name.startsWith(QLatin1Char('/')))
        return name;
    for (int i = 0; i < 4; ++i) {
        const QString p = QString::fromLatin1(dirs[i]) + name;
        if (QFile::exists(p))
            return p;
    }
    return name;
}

void WifiManager::parseWireless(WifiStatus *st) const
{
    /* /proc/net/wireless: "wlan0: 0000  70.  -40.  ..." */
    QFile f(QStringLiteral("/proc/net/wireless"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream ts(&f);
    ts.readLine();
    ts.readLine();
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (!line.startsWith(m_iface + QLatin1Char(':')))
            continue;
        const QStringList c = line.split(
            QRegularExpression(QStringLiteral("\\s+")), QString::SkipEmptyParts);
        if (c.size() >= 2) {
            QString q = c.at(1);
            q.remove(QLatin1Char('.'));
            st->quality = q.toInt();
        }
        break;
    }
    f.close();
}

void WifiManager::parseSsid(const QString &out, WifiStatus *st) const
{
    const QStringList lines = out.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString l = lines.at(i).trimmed();
        if (l.startsWith(QLatin1String("ssid="))) {
            st->ssid = l.mid(5).trimmed();
            break;
        }
    }
}

/* Second half of the async probe: "wpa_cli signal_poll" gives RSSI. This board
 * has no /proc/net/wireless (no WEXT), so the quality figure has to come from
 * wpa_supplicant. RSSI -30 dBm = 100%, -90 dBm = 0% (rough but monotonic). */
void WifiManager::parseRssi(const QString &out, WifiStatus *st) const
{
    const QStringList lines = out.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString l = lines.at(i).trimmed();
        if (!l.startsWith(QLatin1String("RSSI=")))
            continue;
        bool ok = false;
        const int rssi = l.mid(5).trimmed().toInt(&ok);
        if (!ok)
            continue;
        int q = (rssi + 90) * 100 / 60;
        st->quality = qBound(0, q, 100);
        break;
    }
}

/* Asynchronous probe: "wpa_cli status" (SSID) + "wpa_cli signal_poll" (RSSI) in
 * one child process. The periodic poll runs on the GUI thread, so it must never
 * wait for wpa_cli (it can take >1s on this board). */
void WifiManager::onStaFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString out = QString::fromLocal8Bit(m_sta.readAll());
    if (status != QProcess::NormalExit || exitCode != 0)
        return;
    /* the two halves are separated by the marker echoed between them */
    const int sep = out.indexOf(QLatin1String(kStaSep));
    const QString statusOut = sep >= 0 ? out.left(sep) : out;
    const QString signalOut = sep >= 0 ? out.mid(sep + int(qstrlen(kStaSep)))
                                       : QString();

    WifiStatus st = m_st;
    st.ssid.clear();
    parseSsid(statusOut, &st);
    if (!signalOut.isEmpty())
        parseRssi(signalOut, &st);

    if (st.ssid == m_st.ssid && st.quality == m_st.quality)
        return;
    m_st.ssid = st.ssid;
    m_st.quality = st.quality;
    m_st.state = !m_st.linkUp ? QStringLiteral("down")
                 : !m_st.hasIp ? QStringLiteral("no ip")
                 : m_st.ssid.isEmpty() ? QStringLiteral("online")
                                       : m_st.ssid;
    emit statusChanged(m_st);
}

void WifiManager::refresh()
{
    WifiStatus st;
    st.iface = m_iface;
    st.ssid = m_st.ssid;      /* keep the last known SSID until the async
                               * probe (or a lost lease) says otherwise */

    const QString oper = readFirstLine(
        QStringLiteral("/sys/class/net/%1/operstate").arg(m_iface));
    st.linkUp = (oper == QLatin1String("up"));

    /* IPv4 without spawning "ip": QtNetworking reads the same kernel state and
     * returns immediately (spawning a process here would freeze the UI).
     * Only the boolean is kept - the address itself is never stored, shown or
     * logged (a public LCD must not advertise the board's address). */
    const QNetworkInterface ni =
        QNetworkInterface::interfaceFromName(m_iface);
    if (ni.isValid()) {
        const QList<QNetworkAddressEntry> addrs = ni.addressEntries();
        for (int i = 0; i < addrs.size(); ++i) {
            if (addrs.at(i).ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            st.hasIp = true;
            break;
        }
    }
    parseWireless(&st);
    if (!st.hasIp)
        st.ssid.clear();      /* no lease: the old SSID is meaningless */

    st.state = !st.linkUp ? QStringLiteral("down")
               : st.hasIp ? QStringLiteral("online")
                          : QStringLiteral("no ip");
    if (st.hasIp && !st.ssid.isEmpty())
        st.state = st.ssid;

    const bool changed = (st.linkUp != m_st.linkUp) || (st.hasIp != m_st.hasIp) ||
                         (st.ssid != m_st.ssid) || (st.quality != m_st.quality);
    m_st = st;
    if (changed)
        emit statusChanged(m_st);

    /* One async probe at a time, only while there is a link. Both wpa_cli
     * queries run in ONE child via /bin/sh (SSID + RSSI, split by kStaSep). */
    if (m_st.hasIp && m_sta.state() == QProcess::NotRunning) {
        const QString wpa = toolPath(QStringLiteral("wpa_cli"));
        const QString script = wpa + QStringLiteral(" -i ") + m_iface +
                               QStringLiteral(" status; printf '%1\\n'; ")
                                   .arg(QString::fromLatin1(kStaSep)) +
                               wpa + QStringLiteral(" -i ") + m_iface +
                               QStringLiteral(" signal_poll");
        m_sta.start(toolPath(QStringLiteral("sh")),
                    QStringList() << QStringLiteral("-c") << script);
    }
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */

QStringList WifiManager::parseIwScan(const QString &out)
{
    QStringList ssids;
    const QStringList lines = out.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString l = lines.at(i).trimmed();
        if (!l.startsWith(QLatin1String("SSID:")))
            continue;
        const QString s = l.mid(5).trimmed();
        if (!s.isEmpty() && !ssids.contains(s))
            ssids << s;
    }
    return ssids;
}

void WifiManager::finishScan(const QStringList &ssids)
{
    emit scanFinished(ssids);
    if (ssids.isEmpty())
        emit eventMessage(QStringLiteral(
            "wifi: scan returned nothing (no iw/wpa_cli, or wlan0 down)"));
    else
        emit eventMessage(QStringLiteral("wifi: %1 network(s) found")
                              .arg(ssids.size()));
}

void WifiManager::scan()
{
    if (m_scan.state() != QProcess::NotRunning) {
        emit eventMessage(QStringLiteral("wifi: scan already running"));
        return;
    }
    emit eventMessage(QStringLiteral("wifi: scanning on %1 ...").arg(m_iface));
    m_scan.start(toolPath(QStringLiteral("iw")),
                 QStringList() << QStringLiteral("dev") << m_iface
                               << QStringLiteral("scan"));
}

void WifiManager::onScanFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString out = QString::fromLocal8Bit(m_scan.readAll());
    if (status == QProcess::NormalExit && exitCode == 0) {
        const QStringList ssids = parseIwScan(out);
        if (!ssids.isEmpty()) {
            finishScan(ssids);
            return;
        }
    }
    /* no iw (or it failed): ask wpa_supplicant, then read its results a bit
     * later - "wpa_cli scan" returns immediately and fills results async. */
    int rc = 0;
    runSync(toolPath(QStringLiteral("wpa_cli")),
            QStringList() << QStringLiteral("-i") << m_iface
                          << QStringLiteral("scan"),
            1200, &rc);
    QTimer::singleShot(3000, this, [this]() {
        int rc2 = 0;
        const QString res = runSync(toolPath(QStringLiteral("wpa_cli")),
                                    QStringList()
                                        << QStringLiteral("-i") << m_iface
                                        << QStringLiteral("scan_results"),
                                    1500, &rc2);
        QStringList ssids;
        const QStringList lines = res.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const QStringList c = lines.at(i).split(QLatin1Char('\t'));
            if (c.size() < 5)
                continue;
            const QString s = c.at(4).trimmed();
            if (!s.isEmpty() && !ssids.contains(s))
                ssids << s;
        }
        finishScan(ssids);
    });
}

/* ------------------------------------------------------------------ */
/* applying a new configuration                                        */

bool WifiManager::writeConf(const QString &ssid, const QString &psk) const
{
    const QString path = confPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&out);
    ts.setCodec("UTF-8");                 /* SSIDs may be non-ASCII */
    ts << "# EdgeParking - written by the park_ui settings page\n";
    ts << "ctrl_interface=/var/run/wpa_supplicant\n";
    ts << "update_config=1\n";
    ts << "ap_scan=1\n";                  /* same globals as the vendor file */
    ts << "network={\n";
    ts << "\tssid=\"" << wpaEscape(ssid) << "\"\n";
    if (psk.isEmpty()) {
        ts << "\tkey_mgmt=NONE\n";
    } else {
        ts << "\tpsk=\"" << wpaEscape(psk) << "\"\n";
        ts << "\tkey_mgmt=WPA-PSK\n";
    }
    ts << "}\n";
    ts.flush();
    return out.commit();
}

/* wpa_supplicant.conf strings are quoted: backslash and quote must be escaped
 * or a passphrase containing them would corrupt the file. */
QString WifiManager::wpaEscape(const QString &s)
{
    QString out;
    out.reserve(s.size() + 4);
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

bool WifiManager::backupConf() const
{
    const QString p = confPath();
    if (!QFile::exists(p))
        return false;
    const QString b = backupPath();
    QFile::remove(b);
    return QFile::copy(p, b);
}

bool WifiManager::restoreConf() const
{
    const QString b = backupPath();
    if (!QFile::exists(b))
        return false;
    const QString p = confPath();
    QFile::remove(p);
    return QFile::copy(b, p);
}

/* One shell script performs the vendor's three steps. This board has no pkill,
 * so the old wpa_supplicant is located through /proc (skipping $$ = this sh).
 * Every tool is called by absolute path: this rootfs keeps ip/udhcpc/
 * wpa_supplicant/iw in /sbin, which is not guaranteed to be on the PATH a
 * systemd unit hands to park_ui. */
QString WifiManager::buildScript() const
{
    const QString ip = toolPath(QStringLiteral("ip"));
    const QString wpa = toolPath(QStringLiteral("wpa_supplicant"));
    const QString udh = toolPath(QStringLiteral("udhcpc"));

    QString s;
    s += QStringLiteral("'%1' link set %2 up; ").arg(ip, m_iface);
    s += QStringLiteral(
        "for p in /proc/[0-9]*; do pid=${p#/proc/}; [ \"$pid\" = \"$$\" ] && "
        "continue; case \"$(cat $p/cmdline 2>/dev/null)\" in "
        "*wpa_supplicant*) kill $pid 2>/dev/null;; esac; done; ");
    s += QStringLiteral("sleep 1; ");
    s += QStringLiteral("'%1' -B -D nl80211 -i %2 -c '%3'; ")
             .arg(wpa, m_iface, confPath());
    s += QStringLiteral("'%1' -i %2 -n -q -t 5 -T 3; ").arg(udh, m_iface);
    s += QStringLiteral("echo wifi-apply-done");
    return s;
}

void WifiManager::applyConfig(const QString &why)
{
    if (m_apply.state() != QProcess::NotRunning) {
        emit eventMessage(QStringLiteral("wifi: apply already running"));
        return;
    }
    setBusy(true, why);
    emit eventMessage(QStringLiteral(
        "wifi: apply (%1): ip link up -> wpa_supplicant -> udhcpc").arg(why));
    /* Arm the hard deadline BEFORE starting the script: if sh/udhcpc hangs we
     * must still roll back and re-enable the buttons (P7 safety net). */
    m_watchdog.start();
    m_deadline.start(rollbackAfterMs());
    m_apply.start(QStringLiteral("/bin/sh"),
                  QStringList() << QStringLiteral("-c") << buildScript());
}

void WifiManager::onApplyFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString out = QString::fromLocal8Bit(m_apply.readAll()).trimmed();
    emit eventMessage(QStringLiteral("wifi: %1 done rc=%2 (%3) %4")
                          .arg(m_bringUp ? QStringLiteral("bring-up")
                                         : QStringLiteral("apply"))
                          .arg(exitCode)
                          .arg(status == QProcess::NormalExit
                                   ? QStringLiteral("normal")
                                   : QStringLiteral("crashed"))
                          .arg(out.right(60)));
    if (m_bringUp) {
        /* Manual bring-up: the config on disk was not touched, so there is
         * nothing to roll back - only report what the link looks like now. */
        m_bringUp = false;
        m_watchdog.stop();
        m_deadline.stop();
        refresh();
        setBusy(false, QString());
        if (m_st.hasIp)
            emit resultMessage(true, m_st.ssid.isEmpty()
                                         ? QStringLiteral("link up")
                                         : QStringLiteral("link up (ssid %1)")
                                               .arg(m_st.ssid));
        else
            emit resultMessage(false, QStringLiteral(
                "bring-up ran but no lease yet - check ssid/password or the AP"));
        emit statusChanged(m_st);
        return;
    }
    settle(QStringLiteral("apply"));
}

/* Save-only path (2026-09-11 user request: "saving the network settings must
 * update /etc/wpa_supplicant.conf"). CONNECT already writes the file, but it
 * then applies it and - by design - rolls the file back when the new network
 * gives no lease within 20 s; an operator who only wanted to fix a typo in the
 * file saw the old content come back. This writes the same layout and stops
 * there, so what you typed is what is on disk.
 *
 * The first backup is preserved: repeated saves must not overwrite the vendor
 * original that the CONNECT rollback path relies on. */
void WifiManager::saveConfig(const QString &ssid, const QString &psk)
{
    const QString s = ssid.trimmed();
    if (s.isEmpty()) {
        emit resultMessage(false, QStringLiteral("ssid is empty"));
        return;
    }
    if (!psk.isEmpty() && (psk.size() < 8 || psk.size() > 63)) {
        emit resultMessage(false,
                           QStringLiteral("wpa2 passphrase must be 8..63 chars"));
        return;
    }
    if (!QFile::exists(backupPath())) {
        if (backupConf())
            emit eventMessage(QStringLiteral("wifi: backup saved to %1")
                                  .arg(backupPath()));
        else
            emit eventMessage(QStringLiteral(
                "wifi: no %1 to back up (first configuration?)").arg(confPath()));
    }
    if (!writeConf(s, psk)) {
        emit resultMessage(false, QStringLiteral("cannot write %1")
                                      .arg(confPath()));
        return;
    }
    refresh();
    emit eventMessage(QStringLiteral("wifi: %1 updated (ssid=%2%s)")
                          .arg(confPath(), s,
                               psk.isEmpty() ? QStringLiteral(", open network")
                                             : QString()));
    emit resultMessage(true, QStringLiteral(
        "saved to %1 - press CONNECT (or bring up) to activate").arg(confPath()));
    emit statusChanged(m_st);
}

/* Manual bring-up with the CURRENT /etc/wpa_supplicant.conf: exactly the three
 * vendor commands (ip link set up -> wpa_supplicant -B -D nl80211 -> udhcpc),
 * run asynchronously, with no config write and no rollback. Needed because the
 * link is otherwise only raised at boot (wifi-up.service) or when the operator
 * presses CONNECT, and a board installed before wifi-up.service existed
 * (or whose boot sync failed) has no way to raise wlan0 from the panel. */
void WifiManager::bringUp()
{
    if (m_apply.state() != QProcess::NotRunning) {
        emit eventMessage(QStringLiteral(
            "wifi: busy with another apply - bring-up ignored"));
        return;
    }
    if (!QFile::exists(confPath())) {
        emit resultMessage(false, QStringLiteral(
            "no %1 yet - set SSID/password and press CONNECT first")
                                   .arg(confPath()));
        return;
    }
    m_bringUp = true;
    m_rollbackArmed = false;
    m_pendingSsid.clear();
    setBusy(true, QStringLiteral("bring up"));
    emit eventMessage(QStringLiteral(
        "wifi: bring-up with %1 (ip link up -> wpa_supplicant -> udhcpc)")
                          .arg(confPath()));
    /* Hard cap only: no rollback timer, the config is not ours to restore. */
    m_deadline.start(rollbackAfterMs());
    m_apply.start(QStringLiteral("/bin/sh"),
                  QStringList() << QStringLiteral("-c") << buildScript());
}

/* One decision point for "did the new configuration work?".
 * Called when the apply script ends and from the watchdog/deadline timer, so
 * a hung script cannot leave the UI busy forever. */
void WifiManager::settle(const QString &why)
{
    refresh();
    if (m_bringUp) {
        /* bring-up: report only, never touch the file (see bringUp()) */
        if (m_apply.state() != QProcess::NotRunning)
            return;                    /* its finished handler reports */
        m_bringUp = false;
        m_watchdog.stop();
        m_deadline.stop();
        setBusy(false, QString());
        emit resultMessage(m_st.hasIp,
                           m_st.hasIp
                               ? QStringLiteral("link up")
                               : QStringLiteral("bring-up (%1) gave no lease - "
                                                "check ssid/password or the AP")
                                     .arg(why));
        emit statusChanged(m_st);
        return;
    }
    if (m_st.hasIp) {
        m_watchdog.stop();
        m_deadline.stop();
        m_rollbackArmed = false;
        setBusy(false, QString());
        emit resultMessage(true, m_st.ssid.isEmpty()
                                     ? QStringLiteral("connected")
                                     : QStringLiteral("connected to %1")
                                           .arg(m_st.ssid));
        return;
    }
    if (m_deadline.isActive())
        return;                       /* still inside the rollback window */
    if (m_apply.state() != QProcess::NotRunning)
        return;                       /* its finished handler settles this */

    m_watchdog.stop();
    if (m_rollbackArmed) {            /* the rollback did not help either */
        setBusy(false, QString());
        emit resultMessage(false, QStringLiteral(
            "still no ip lease after rollback - check ssid/password"));
        return;
    }
    m_rollbackArmed = true;
    if (restoreConf()) {
        emit eventMessage(QStringLiteral(
            "wifi: no lease in %1s (%2), restoring the previous config")
                              .arg(rollbackAfterMs() / 1000)
                              .arg(why));
        emit resultMessage(false, QStringLiteral(
            "no ip lease - previous wifi config restored"));
        m_deadline.start(rollbackAfterMs());   /* window for the retry */
        applyConfig(QStringLiteral("rollback"));
    } else {
        setBusy(false, QString());
        emit resultMessage(false, QStringLiteral(
            "no ip lease and no backup to restore"));
    }
}

void WifiManager::onWatchdog()
{
    /* while the script runs, keep the "busy" text but do not decide yet */
    if (m_apply.state() != QProcess::NotRunning) {
        refresh();
        return;
    }
    settle(QStringLiteral("watchdog"));
}

void WifiManager::connectTo(const QString &ssid, const QString &psk)
{
    const QString s = ssid.trimmed();
    if (s.isEmpty()) {
        emit resultMessage(false, QStringLiteral("ssid is empty"));
        return;
    }
    if (!psk.isEmpty() && (psk.size() < 8 || psk.size() > 63)) {
        emit resultMessage(false,
                           QStringLiteral("wpa2 passphrase must be 8..63 chars"));
        return;
    }
    if (backupConf())
        emit eventMessage(QStringLiteral("wifi: backup saved to %1")
                              .arg(backupPath()));
    else
        emit eventMessage(QStringLiteral(
            "wifi: no %1 to back up (first configuration?)").arg(confPath()));

    if (!writeConf(s, psk)) {
        /* refuse to apply: the file on disk is still the old network, so a
         * "success" here would be a lie */
        emit resultMessage(false,
                           QStringLiteral("cannot write %1").arg(confPath()));
        return;
    }
    m_pendingSsid = s;
    m_rollbackArmed = false;
    emit eventMessage(QStringLiteral("wifi: connecting to '%1'").arg(s));
    applyConfig(QStringLiteral("connect"));
}
void WifiManager::disconnectFrom()
{
    int rc = 0;
    runSync(toolPath(QStringLiteral("wpa_cli")),
            QStringList() << QStringLiteral("-i") << m_iface
                          << QStringLiteral("disconnect"),
            2500, &rc);
    runSync(toolPath(QStringLiteral("ip")),
            QStringList() << QStringLiteral("link") << QStringLiteral("set")
                          << m_iface << QStringLiteral("down"),
            2500, &rc);
    refresh();
    emit eventMessage(QStringLiteral("wifi: disconnected"));
    emit resultMessage(true, QStringLiteral("wlan0 down"));
}

void WifiManager::restoreBackup()
{
    if (!QFile::exists(backupPath())) {
        emit resultMessage(false, QStringLiteral("no backup to restore"));
        return;
    }
    if (!restoreConf()) {
        emit resultMessage(false, QStringLiteral("restore failed"));
        return;
    }
    m_rollbackArmed = false;
    emit eventMessage(QStringLiteral("wifi: previous config restored"));
    applyConfig(QStringLiteral("restore"));
}

void WifiManager::abortRollback()
{
    m_watchdog.stop();
    m_deadline.stop();
    m_rollbackArmed = false;
    setBusy(false, QString());
    emit eventMessage(QStringLiteral("wifi: rollback cancelled by operator"));
}

void WifiManager::setBusy(bool busy, const QString &what)
{
    if (m_busy == busy && what.isEmpty())
        return;
    m_busy = busy;
    emit busyChanged(busy, what);
}

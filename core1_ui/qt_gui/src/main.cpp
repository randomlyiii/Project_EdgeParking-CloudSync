#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QSslSocket>
#include <QTimer>

#include "cloud_client.h"
#include "cloud_settings.h"
#include "ipc_reader.h"
#include "ipc_writer.h"
#include "k210_link.h"
#include "mainwindow.h"
#include "settingspage.h"
#include "wifi_manager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("park_ui");
    QApplication::setOrganizationName("EdgeParking");

    /* CJK-capable default family; linuxfb resolves fonts via QT_QPA_FONTDIR */
    QFont f("Noto Sans CJK SC");
    QApplication::setFont(f);

    QCommandLineParser cli;
    cli.setApplicationDescription(
        "park_ui - parking lot UI (PhaseMd/11). Qt 5.12.x, linuxfb target.");
    cli.addHelpOption();
    /* k210 link */
    QCommandLineOption optDev(QStringList() << "d" << "dev",
                              "K210 serial device (default $PARK_UI_TTY or "
                              "/dev/ttyACM0)", "dev");
    QCommandLineOption optBaud(QStringList() << "b" << "baud",
                               "serial baud (default 115200)", "baud");
    QCommandLineOption optMode(QStringList() << "m" << "mode",
                               "link mode: auto|text|binary|file|none "
                               "(default auto)", "mode");
    QCommandLineOption optFile(QStringList() << "f" << "file",
                               "image file or directory for --mode file",
                               "path");
    QCommandLineOption optDemo("demo",
                               "demo mode: on|off|auto (default auto = "
                               "simulate when /park_shm absent)", "demo");
    QCommandLineOption optEvt("eventfd",
                              "pre-opened eventfd number for IPC state "
                              "events (P6-02), -1 = poll shm (default)", "fd",
                              "-1");
    cli.addOptions({optDev, optBaud, optMode, optFile, optDemo, optEvt});
    cli.process(app);

    const QString dev = cli.value(optDev).isEmpty()
        ? qEnvironmentVariable("PARK_UI_TTY", "/dev/ttyACM0")
        : cli.value(optDev);
    const int baud = cli.value(optBaud).toInt() > 0
        ? cli.value(optBaud).toInt() : 115200;
    QString mode = cli.value(optMode).toLower();
    if (mode.isEmpty())
        mode = "auto";
    const QString file = cli.value(optFile);
    /* Careful: a missing --eventfd must NOT become fd 0 (stdin): under systemd
     * stdin is /dev/null, always readable, and a QSocketNotifier on it spins a
     * core. The option carries an explicit "-1" default for this reason. */
    const int evtFd = cli.isSet(optEvt) ? cli.value(optEvt).toInt() : -1;

    IpcReader::DemoMode demoMode = IpcReader::DemoAuto;
    const QString demo = cli.value(optDemo).toLower();
    if (demo == "on")
        demoMode = IpcReader::DemoForce;
    else if (demo == "off")
        demoMode = IpcReader::DemoOff;

    qRegisterMetaType<IpcSnapshot>("IpcSnapshot");

    K210Link link;
    MainWindow win;
    win.setLink(&link);

    IpcReader ipc;
    win.setIpc(&ipc);

    IpcWriter writer;
    QObject::connect(&ipc, &IpcReader::snapshotChanged,
                     &win, &MainWindow::onSnapshot);
    QObject::connect(&ipc, &IpcReader::eventMessage,
                     &win, &MainWindow::pushEvent);
    QObject::connect(&ipc, &IpcReader::platePopup,
                     &win, &MainWindow::showPlatePopup);
    QObject::connect(&link, &K210Link::linkUp,
                     &win, &MainWindow::onLinkUp);
    QObject::connect(&link, &K210Link::recogResult,
                     &win, &MainWindow::onRecogResult);
    QObject::connect(&link, &K210Link::recogFailed,
                     &win, &MainWindow::onRecogFailed);
    QObject::connect(&link, &K210Link::busyChanged,
                     &win, &MainWindow::onK210Busy);
    /* Core1 business write-end: K210 results -> shm, UI gate hotkeys -> pulse */
    QObject::connect(&link, &K210Link::recogResult,
                     &writer, &IpcWriter::onRecogResult);
    QObject::connect(&link, &K210Link::recogFailed,
                     &writer, &IpcWriter::onRecogFailed);
    QObject::connect(&win, &MainWindow::gateRequested, &writer,
                     [&writer](bool open) {
                         if (open) writer.requestGateOpen();
                         else      writer.requestGateClose();
                     });
    QObject::connect(&writer, &IpcWriter::eventMessage,
                     &win, &MainWindow::pushEvent);
    QObject::connect(&writer, &IpcWriter::cloudPendingChanged,
                     &win, &MainWindow::onCloudPending);
    QObject::connect(&writer, &IpcWriter::snapshotRefreshRequested,
                     &ipc, &IpcReader::onTick);

    ipc.start(demoMode, evtFd);
    writer.start();
    link.start(dev, baud, mode, file);

    /* ===================== step 7: cloud + network =====================
     * Everything cloud related lives in Core1 only (PhaseMd/08: Core0 is
     * forbidden from making cloud requests). Three objects:
     *   CloudSettings - persisted config (thresholds/endpoint/model/key)
     *   CloudClient   - async HTTPS request, single shot, timeout + retry 1
     *   WifiManager   - wlan0 status + SSID/password apply with rollback
     * plus the full-screen operator page that drives them. */
    CloudSettings cset;
    QString cwarn;
    const bool cfile = cloud_settings_load(&cset, &cwarn);
    qWarning("cloud: config %s (%s) api=%s model=%s key=%s timeout=%dms retry=%d",
             qPrintable(cloud_settings_path()),
             cfile ? "loaded" : "defaults",
             qPrintable(cset.apiBase), qPrintable(cset.model),
             qPrintable(cloud_settings_mask_key(cset.apiKey)),
             cset.timeoutMs, cset.retry);
    if (!cwarn.isEmpty())
        qWarning("cloud: %s", qPrintable(cwarn));
    if (!cloud_settings_has_key(cset))
        qWarning("cloud: no API key - set it on the LCD settings page "
                 "($PARK_CLOUD_CONF=%s)", qPrintable(cloud_settings_path()));
    /* Decisive TLS diagnostic: this rootfs ships libssl.so.1.1 but no CA
     * bundle, so both "does Qt have TLS at all" and the CA lookup are worth
     * logging once at startup. */
    const QString caPath = CloudClient::caBundlePathFor(cset);
    qWarning("cloud: Qt TLS supportsSsl=%s (%s), CA=%s",
             QSslSocket::supportsSsl() ? "yes" : "NO",
             qPrintable(QSslSocket::sslLibraryBuildVersionString()),
             caPath.isEmpty()
                 ? "NONE - install /etc/park/ca.pem or set insecure_tls=1"
                 : qPrintable(caPath));
    /* ... and log the proxy situation: a stray https_proxy in the service
     * environment makes Qt connect to a dead proxy and burn the whole timeout,
     * while a direct socket (python) still works - exactly the "works for
     * python, times out in the UI" signature. */
    qWarning("cloud: proxy mode=%s (env https_proxy='%s' http_proxy='%s'), "
             "transport=%s",
             qPrintable(CloudClient::proxyModeText(cset)),
             qgetenv("https_proxy").constData(),
             qgetenv("http_proxy").constData(),
             qPrintable(CloudClient::transportText(cset)));

    CloudClient cloud;
    cloud.setSettings(cset);
    WifiManager wifi;
    /* The WiFi interface is normally wlan0; PARK_UI_WIFI lets a board with a
     * differently named interface be brought up without a rebuild (it was
     * already documented in G7_ACCEPTANCE / deploy/README). */
    wifi.setInterface(qEnvironmentVariable("PARK_UI_WIFI", "wlan0"));

    SettingsPage settings(&cset, &cloud, &wifi);
    win.setSettingsPage(&settings);

    /* Manual cloud recheck: bottom-bar button and the settings-page button. */
    const auto requestCloudCheck = [&](const QString &reason) {
        QImage frame;
        if (!link.latestFrame(&frame)) {
            win.pushEvent(QStringLiteral(
                "cloud: no K210 frame yet, recheck skipped"));
            return;
        }
        cloud.recognize(frame, cset.writeback, reason);
    };
    QObject::connect(&win, &MainWindow::cloudCheckRequested, &cloud,
                     [&requestCloudCheck]() {
                         requestCloudCheck(QStringLiteral("manual"));
                     });
    QObject::connect(&settings, &SettingsPage::manualCloudRequested, &cloud,
                     [&requestCloudCheck]() {
                         requestCloudCheck(QStringLiteral("manual-settings"));
                     });

    /* Low edge confidence / recognition failure -> ask the cloud (P7-06).
     * IpcWriter owns cloud_pending; we either clear it or feed it a result. */
    QObject::connect(&writer, &IpcWriter::cloudFallbackRequested, &cloud,
                     [&](const QString &reason) {
                         if (!cset.autoFallback) {
                             win.pushEvent(QStringLiteral(
                                 "cloud: auto fallback off (") + reason +
                                 QStringLiteral(") - cloud_pending cleared"));
                             writer.clearCloudPending();
                             return;
                         }
                         QImage frame;
                         if (!link.latestFrame(&frame)) {
                             win.pushEvent(QStringLiteral(
                                 "cloud: fallback failed - no K210 frame"));
                             writer.clearCloudPending();
                             return;
                         }
                         cloud.recognize(frame, true, reason);
                     });

    /* Cloud answers: write back to /park_shm as result_source=1, or only log
     * when the operator switched write-back off. */
    QObject::connect(&cloud, &CloudClient::finished, &writer,
                     [&](const QString &plate, double conf, int httpStatus,
                         qint64 ms, bool writeback) {
                         if (writeback) {
                             qWarning("cloud: accepted '%s' conf=%.2f in %lld ms"
                                      " -> write-back",
                                      qPrintable(plate), conf,
                                      static_cast<long long>(ms));
                             writer.onCloudResult(plate, conf);
                             return;
                         }
                         qWarning("cloud: '%s' conf=%.2f http=%d in %lld ms"
                                  " (no write-back)",
                                  qPrintable(plate), conf, httpStatus,
                                  static_cast<long long>(ms));
                         win.pushEvent(QStringLiteral(
                             "cloud: '%1' conf=%2 http=%3 in %4ms (no write-back)")
                                           .arg(plate)
                                           .arg(conf, 0, 'f', 2)
                                           .arg(httpStatus)
                                           .arg(ms));
                         win.showPlatePopup(plate, conf, 1, false);
                     });
    QObject::connect(&cloud, &CloudClient::failed, &writer,
                     [&](const QString &reason, const QString &detail) {
                         /* journal as well as the ticker: remote debugging
                          * needs the classified reason (timeout / 401 / tls)
                          * AND the link state - a missing WiFi lease is by far
                          * the most common cause of "the cloud stopped working"
                          * (see wifi-up.service). */
                         const WifiStatus ws = wifi.status();
                         qWarning("cloud: FAILED %s (%s) [wifi:lease=%s ssid=%s]",
                                  qPrintable(reason), qPrintable(detail),
                                  ws.hasIp ? "yes" : "no",
                                  qPrintable(ws.ssid));
                         QString line = QStringLiteral("cloud: FAILED %1 (%2)")
                                            .arg(reason, detail);
                         if (!ws.hasIp)
                             line += QStringLiteral(
                                 " - no WiFi lease: systemctl restart wifi-up");
                         else if (reason.contains(QLatin1String("certificate")))
                             line += QStringLiteral(
                                 " - check date -u (no RTC) and /etc/park/ca.pem"
                                 " (or set insecure_tls=1 for a demo)");
                         win.pushEvent(line);
                         writer.clearCloudPending();  /* no-op if not pending */
                     });
    QObject::connect(&cloud, &CloudClient::unreadable, &writer,
                     [&](const QString &detail, qint64 ms) {
                         qWarning("cloud: unreadable (%s) in %lld ms",
                                  qPrintable(detail), static_cast<long long>(ms));
                         win.pushEvent(QStringLiteral(
                             "cloud: unreadable (%1) in %2ms").arg(detail).arg(ms));
                         writer.clearCloudPending();  /* no-op if not pending */
                     });
    QObject::connect(&cloud, &CloudClient::eventMessage,
                     &win, &MainWindow::pushEvent);
    QObject::connect(&cloud, &CloudClient::statsChanged, &win, [&]() {
        const CloudStats st = cloud.stats();
        /* chip: busy > last error (short form) > idle/ok/no key */
        QString text;
        if (st.busy)
            text = QStringLiteral("...");
        else if (!st.lastError.isEmpty())
            text = st.lastError.left(16);
        else
            text = cloud.statusText();
        win.setCloudChip(text, st.busy || st.lastError.isEmpty());
    });

    /* WiFi: 2s status poll -> WIFI chip; apply/scan happen on the page. */
    QTimer wifiTimer;
    QObject::connect(&wifiTimer, &QTimer::timeout, &wifi, &WifiManager::refresh);
    QObject::connect(&wifi, &WifiManager::statusChanged, &win,
                     [&](const WifiStatus &st) {
                         QString text = st.state;
                         if (st.hasIp) {
                             /* SSID only - the leased address is never shown
                              * (nor kept; see wifi_manager.h) */
                             text = st.ssid.isEmpty()
                                        ? QStringLiteral("online")
                                        : st.ssid;
                         }
                         if (text.isEmpty())
                             text = QStringLiteral("down");
                         win.setWifiChip(text, st.hasIp);
                     });
    QObject::connect(&wifi, &WifiManager::eventMessage,
                     &win, &MainWindow::pushEvent);

    /* the page shows Core0's authoritative threshold read-only (spec 5.5.1) */
    QObject::connect(&win, &MainWindow::coreThresholdChanged, &settings,
                     [&settings](double thr, bool valid) {
                         settings.setCoreThreshold(thr, valid);
                     });

    wifiTimer.start(2000);
    wifi.refresh();

    win.show();
    const int rc = app.exec();

    wifiTimer.stop();
    cloud.cancel();
    link.stop();
    writer.stop();
    ipc.stop();
    return rc;
}

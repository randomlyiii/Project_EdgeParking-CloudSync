#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
/* WifiManager - wlan0 status + reconfiguration for the LCD (step 7 extras).
 *
 * The board's only network link is the onboard WiFi (Task.md: single network
 * link, WiFi only), so the panel must be able to show whether it is up and to
 * change the SSID/password using the vendor's recipe:
 *
 *     ip link set wlan0 up
 *     wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf
 *     udhcpc -i wlan0
 *
 * Safety: the operator very likely reaches this panel over that same WiFi
 * (ssh). Every apply therefore backs the old /etc/wpa_supplicant.conf up to
 * .bak and arms a watchdog: if no IPv4 address shows up within
 * ROLLBACK_AFTER_MS the previous configuration is restored and re-applied.
 *
 * Everything runs through /bin/sh + busybox tools that exist on this image
 * (ip/udhcpc/wpa_supplicant/wpa_cli/iw-if-present). There is NO pgrep/pkill on
 * this board, so process lookup falls back to a /proc scan.
 */
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

/* The leased address and the gateway are deliberately NOT members of this
 * structure. Everything held here ends up on the LCD and/or in the journal, and
 * a panel that publishes the board's own address invites casual tampering. The
 * logic only ever needs the boolean "do we have a lease", so keep it that way:
 * no address is parsed, stored, displayed or logged anywhere in Core1. */
struct WifiStatus {
    QString iface;
    bool    linkUp = false;   /* operstate up */
    bool    hasIp = false;    /* got an IPv4 lease (the address is not kept) */
    QString ssid;
    int     quality = -1;     /* RSSI-derived 0..100, -1 unknown */
    QString state;            /* ASCII summary for the UI chip */
};

class WifiManager : public QObject
{
    Q_OBJECT
public:
    explicit WifiManager(QObject *parent = nullptr);

    void setInterface(const QString &iface);
    QString interface() const { return m_iface; }
    WifiStatus status() const { return m_st; }
    bool busy() const { return m_busy; }
    static QString confPath();
    static QString backupPath();
    /* ROLLBACK_AFTER_MS: how long a new config may take to get a lease */
    static int rollbackAfterMs() { return 20000; }

public slots:
    void refresh();                                   /* cheap, call every 2s */
    void scan();                                      /* async, may be empty */
    void connectTo(const QString &ssid, const QString &psk);
    void disconnectFrom();
    void restoreBackup();
    void abortRollback();

signals:
    void statusChanged(const WifiStatus &s);
    void scanFinished(const QStringList &ssids);
    void busyChanged(bool busy, const QString &what);
    void resultMessage(bool ok, const QString &detail);
    void eventMessage(const QString &line);

private slots:
    void onWatchdog();
    void onApplyFinished(int exitCode, QProcess::ExitStatus status);
    void onScanFinished(int exitCode, QProcess::ExitStatus status);
    void onStaFinished(int exitCode, QProcess::ExitStatus status);

private:
    static QString runSync(const QString &program, const QStringList &args,
                           int timeoutMs, int *exitCode = nullptr);
    static QString wpaEscape(const QString &s);
    /* Absolute path of a board tool (this rootfs keeps the wifi tools in
     * /sbin, which is not guaranteed to be on a systemd unit's PATH). */
    static QString toolPath(const QString &name);
    void applyConfig(const QString &why);
    /* single decision point: lease there -> done, else rollback (or give up) */
    void settle(const QString &why);
    QString buildScript() const;
    bool writeConf(const QString &ssid, const QString &psk) const;
    bool backupConf() const;
    bool restoreConf() const;
    void setBusy(bool busy, const QString &what);
    void parseWireless(WifiStatus *st) const;
    void parseSsid(const QString &wpaCliOut, WifiStatus *st) const;
    void parseRssi(const QString &wpaCliOut, WifiStatus *st) const;
    static QStringList parseIwScan(const QString &out);
    void finishScan(const QStringList &ssids);

    /* marker echoed between the two halves of the async wpa_cli probe */
    static const char *const kStaSep;

    QString m_iface = QStringLiteral("wlan0");
    WifiStatus m_st;
    bool m_busy = false;          /* an apply is in flight */
    bool m_rollbackArmed = false;
    QTimer m_watchdog;            /* 2s tick while waiting for a lease */
    QTimer m_deadline;            /* 20s hard limit */
    QString m_pendingSsid;
    QProcess m_apply;             /* runs buildScript() without blocking the UI */
    QProcess m_scan;              /* "iw dev wlan0 scan" */
    QProcess m_sta;               /* async "wpa_cli status" (SSID only) */
};

#endif /* WIFI_MANAGER_H */

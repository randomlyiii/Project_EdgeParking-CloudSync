#ifndef SETTINGSPAGE_H
#define SETTINGSPAGE_H
/* SettingsPage - full-screen operator page (cloud + wifi + diagnostics).
 *
 * Opened from the gear button in the main window. Everything the operator can
 * change lives here:
 *   cloud  : trigger/accept confidence, API base, model, API key (masked),
 *            toggles (auto fallback / result write-back / insecure TLS /
 *            fake result / outage drill), test connection, manual cloud check
 *   wifi   : status, SSID + passphrase (soft keyboard), connect/disconnect,
 *            restore previous config, scan
 *   diag   : systemd units, device nodes, K210 link, shm state
 *
 * All labels are ASCII except a handful of Chinese captions written as UTF-8
 * escapes (the GUI sources must stay pure ASCII - board rule).
 */
#include <QWidget>

#include "cloud_settings.h"
#include "wifi_manager.h"   /* WifiStatus is used in a slot signature (moc) */

class CloudClient;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QPushButton;

class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    SettingsPage(CloudSettings *s, CloudClient *cloud, WifiManager *wifi,
                 QWidget *parent = nullptr);

    /* rebuild every field from the live objects (call before showing) */
    void refreshAll();
    /* show Core0's authoritative conf_threshold (read-only reference) */
    void setCoreThreshold(double thr, bool valid);

signals:
    void closed();
    void manualCloudRequested();   /* "cloud recheck" on this page */

protected:
    void showEvent(QShowEvent *e) override;   /* re-read state on open */

private slots:
    void onTriggerMinus();
    void onTriggerPlus();
    void onAcceptMinus();
    void onAcceptPlus();
    void onEditApiBase();
    void onEditModel();
    void onModelPreset();
    void onEditKey();
    void onToggleChanged();
    void onSave();
    void onCloudStats();
    void onWifiStatus(const WifiStatus &s);
    void onWifiScan(const QStringList &ssids);
    void onWifiBusy(bool busy, const QString &what);
    void onWifiResult(bool ok, const QString &detail);
    void onWifiMessage(const QString &line);
    void onConnect();
    void onUiMessage(const QString &line);
    void onCloudFailed(const QString &reason, const QString &detail);
    void onCloudUnreadable(const QString &detail, qint64 ms);
    void onCloudFinished(const QString &plate, double conf, int status,
                         qint64 ms, bool writeback);
    void refreshDiag();

private:
    QWidget *buildCloudTab();
    QWidget *buildWifiTab();
    QWidget *buildDiagTab();
    void applyToUi();
    void setStatus(const QString &line);
    void save();

    CloudSettings *m_s = nullptr;
    CloudClient *m_cloud = nullptr;
    WifiManager *m_wifi = nullptr;

    QTabWidget *m_tabs = nullptr;
    QLabel *m_lblStatus = nullptr;

    /* cloud tab */
    QLabel *m_lblTrigger = nullptr;
    QLabel *m_lblAccept = nullptr;
    QLabel *m_lblCoreThr = nullptr;
    QLabel *m_lblCloudStat = nullptr;
    QLineEdit *m_edApiBase = nullptr;
    QLineEdit *m_edModel = nullptr;
    QLineEdit *m_edKey = nullptr;
    QCheckBox *m_ckAuto = nullptr;
    QCheckBox *m_ckWriteback = nullptr;
    QCheckBox *m_ckInsecure = nullptr;
    QCheckBox *m_ckFake = nullptr;
    QCheckBox *m_ckOutage = nullptr;

    /* wifi tab */
    QLabel *m_lblWifi = nullptr;
    QLineEdit *m_edSsid = nullptr;
    QLineEdit *m_edPsk = nullptr;
    QListWidget *m_lstSsid = nullptr;
    QLabel *m_lblWifiHint = nullptr;
    QPushButton *m_btnConnect = nullptr;

    /* diag tab */
    QLabel *m_lblDiag = nullptr;
    QLabel *m_lblDiagDev = nullptr;
};

#endif /* SETTINGSPAGE_H */

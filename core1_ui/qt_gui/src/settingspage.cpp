#include "settingspage.h"

#include "cloud_client.h"
#include "softkeyboard.h"
#include "wifi_manager.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QShowEvent>
#include <QSslSocket>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

/* Chinese captions as UTF-8 escapes: every GUI source stays pure ASCII. */
#define TXT_SETTINGS  "\xE8\xAE\xBE\xE7\xBD\xAE"                         /* she zhi   */
#define TXT_CLOUD     "\xE4\xBA\x91\xE7\xAB\xAF"                         /* yun duan  */
#define TXT_NET       "\xE7\xBD\x91\xE7\xBB\x9C"                         /* wang luo  */
#define TXT_DIAG      "\xE8\xAF\x8A\xE6\x96\xAD"                         /* zhen duan */
#define TXT_CLOSE     "\xE5\x85\xB3\xE9\x97\xAD"                         /* guan bi   */
#define TXT_SAVE      "\xE4\xBF\x9D\xE5\xAD\x98"                         /* bao cun   */
#define TXT_TEST      "\xE6\xB5\x8B\xE8\xAF\x95\xE8\xBF\x9E\xE6\x8E\xA5" /* ce shi lian jie */
#define TXT_RECHECK   "\xE5\xA4\x8D\xE6\xA3\x80"                         /* fu jian   */
#define TXT_TRIGGER   "\xE8\xA7\xA6\xE5\x8F\x91"                         /* chu fa    */
#define TXT_ACCEPT    "\xE9\x87\x87\xE7\xBA\xB3"                         /* cai na    */
#define TXT_THRESHOLD "\xE9\x98\x88\xE5\x80\xBC"                         /* yu zhi    */
#define TXT_MODEL     "\xE6\xA8\xA1\xE5\x9E\x8B"                         /* mo xing   */
#define TXT_APIBASE   "\xE5\x9C\xB0\xE5\x9D\x80"                         /* di zhi    */
#define TXT_KEY       "\xE5\xAF\x86\xE9\x92\xA5"                         /* mi yao    */
#define TXT_EDIT      "\xE4\xBF\xAE\xE6\x94\xB9"                         /* xiu gai   */
#define TXT_CONNECT   "\xE8\xBF\x9E\xE6\x8E\xA5"                         /* lian jie  */
#define TXT_DISCONN   "\xE6\x96\xAD\xE5\xBC\x80"                         /* duan kai  */
#define TXT_SCAN      "\xE6\x89\xAB\xE6\x8F\x8F"                         /* sao miao  */
#define TXT_PASS      "\xE5\xAF\x86\xE7\xA0\x81"                         /* mi ma     */
#define TXT_RESTORE   "\xE6\x81\xA2\xE5\xA4\x8D\xE4\xB8\x8A\xE6\xAC\xA1" \
                      "\xE9\x85\x8D\xE7\xBD\xAE"                         /* restore   */
#define TXT_AUTO      "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x85\x9C\xE5\xBA\x95" /* auto fb   */
#define TXT_WRITEBAK  "\xE7\xBB\x93\xE6\x9E\x9C\xE5\x9B\x9E\xE5\x86\x99" /* writeback */
#define TXT_INSECURE  "\xE8\xB7\xB3\xE8\xBF\x87\xE8\xAF\x81\xE4\xB9\xA6" \
                      "\xE6\xA0\xA1\xE9\xAA\x8C"                         /* skip tls  */
#define TXT_FAKE      "\xE5\x81\x87\xE7\xBB\x93\xE6\x9E\x9C\xE6\xBC\x94" \
                      "\xE7\xA4\xBA"                                     /* fake demo */
#define TXT_OUTAGE    "\xE6\x96\xAD\xE7\xBD\x91\xE6\xBC\x94\xE7\xBB\x83" /* outage    */
#define TXT_REFRESH   "\xE5\x88\xB7\xE6\x96\xB0"                         /* shua xin  */
#define TXT_STATE     "\xE7\x8A\xB6\xE6\x80\x81"                         /* zhuang tai*/
#define TXT_SERVICES  "\xE6\x9C\x8D\xE5\x8A\xA1"                         /* fu wu     */
#define TXT_DEVICES   "\xE8\xAE\xBE\xE5\xA4\x87"                         /* she bei   */

static QPushButton *mkButton(const QString &text, QWidget *parent,
                             const char *bg = nullptr, bool checkable = false)
{
    QPushButton *b = new QPushButton(text, parent);
    b->setCheckable(checkable);
    b->setMinimumHeight(46);
    b->setMinimumWidth(120);
    b->setFocusPolicy(Qt::NoFocus);
    if (bg != nullptr)
        b->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1;color:#eceff1;font-size:17px;"
            "border:1px solid #37474f;border-radius:6px;}"
            "QPushButton:pressed{background:#455a64;}"
            "QPushButton:checked{background:#0d47a1;}")
                             .arg(QLatin1String(bg)));
    return b;
}

static QLabel *mkVal(QWidget *parent, const QString &text = QString())
{
    QLabel *l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("QLabel{color:#eceff1;font-size:17px;}"));
    return l;
}

static QLabel *mkCap(QWidget *parent, const QString &text)
{
    QLabel *l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("QLabel{color:#90a4ae;font-size:17px;}"));
    return l;
}

SettingsPage::SettingsPage(CloudSettings *s, CloudClient *cloud,
                           WifiManager *wifi, QWidget *parent)
    : QWidget(parent), m_s(s), m_cloud(cloud), m_wifi(wifi)
{
    setObjectName(QStringLiteral("settingsPage"));
    setStyleSheet(QStringLiteral("QWidget#settingsPage{background:#101418;}"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    QHBoxLayout *head = new QHBoxLayout();
    QLabel *title = new QLabel(QString::fromUtf8(TXT_SETTINGS), this);
    title->setStyleSheet(QStringLiteral("QLabel{color:#eceff1;font-size:24px;"
                                        "font-weight:bold;}"));
    head->addWidget(title);
    head->addStretch(1);
    m_lblStatus = new QLabel(QString(), this);
    m_lblStatus->setStyleSheet(QStringLiteral("QLabel{color:#ffab40;"
                                              "font-size:15px;}"));
    head->addWidget(m_lblStatus);
    QPushButton *close = mkButton(QString::fromUtf8(TXT_CLOSE), this, "#4e342e");
    connect(close, &QPushButton::clicked, this, [this]() { emit closed(); });
    head->addWidget(close);
    root->addLayout(head);

    m_tabs = new QTabWidget(this);
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane{border:1px solid #37474f;}"
        "QTabBar::tab{background:#1c232b;color:#cfd8dc;font-size:18px;"
        "padding:8px 18px;}"
        "QTabBar::tab:selected{background:#263238;color:#eceff1;}"));
    m_tabs->addTab(buildCloudTab(), QString::fromUtf8(TXT_CLOUD));
    m_tabs->addTab(buildWifiTab(), QString::fromUtf8(TXT_NET));
    m_tabs->addTab(buildDiagTab(), QString::fromUtf8(TXT_DIAG));
    root->addWidget(m_tabs, 1);

    if (m_cloud != nullptr) {
        connect(m_cloud, &CloudClient::statsChanged, this,
                &SettingsPage::onCloudStats);
        connect(m_cloud, &CloudClient::eventMessage, this,
                &SettingsPage::onUiMessage);
        connect(m_cloud, &CloudClient::failed, this,
                &SettingsPage::onCloudFailed);
        connect(m_cloud, &CloudClient::unreadable, this,
                &SettingsPage::onCloudUnreadable);
        connect(m_cloud, &CloudClient::finished, this,
                &SettingsPage::onCloudFinished);
    }
    if (m_wifi != nullptr) {
        connect(m_wifi, &WifiManager::statusChanged, this,
                &SettingsPage::onWifiStatus);
        connect(m_wifi, &WifiManager::scanFinished, this,
                &SettingsPage::onWifiScan);
        connect(m_wifi, &WifiManager::busyChanged, this,
                &SettingsPage::onWifiBusy);
        connect(m_wifi, &WifiManager::resultMessage, this,
                &SettingsPage::onWifiResult);
        connect(m_wifi, &WifiManager::eventMessage, this,
                &SettingsPage::onWifiMessage);
    }
    applyToUi();
}

/* ---------------------------------------------------------------- cloud */

QWidget *SettingsPage::buildCloudTab()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *v = new QVBoxLayout(w);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    /* thresholds */
    QHBoxLayout *t1 = new QHBoxLayout();
    t1->addWidget(mkCap(w, QString::fromUtf8(TXT_TRIGGER TXT_THRESHOLD)));
    QPushButton *tMinus = mkButton(QStringLiteral("-"), w);
    QPushButton *tPlus = mkButton(QStringLiteral("+"), w);
    m_lblTrigger = mkVal(w, QStringLiteral("0.60"));
    m_lblTrigger->setMinimumWidth(80);
    m_lblTrigger->setStyleSheet(QStringLiteral("QLabel{color:#69f0ae;"
                                              "font-size:22px;font-weight:bold;}"));
    t1->addWidget(tMinus);
    t1->addWidget(m_lblTrigger);
    t1->addWidget(tPlus);
    t1->addSpacing(24);
    t1->addWidget(mkCap(w, QString::fromUtf8(TXT_ACCEPT TXT_THRESHOLD)));
    QPushButton *aMinus = mkButton(QStringLiteral("-"), w);
    QPushButton *aPlus = mkButton(QStringLiteral("+"), w);
    m_lblAccept = mkVal(w, QStringLiteral("0.50"));
    m_lblAccept->setMinimumWidth(80);
    m_lblAccept->setStyleSheet(QStringLiteral("QLabel{color:#69f0ae;"
                                              "font-size:22px;font-weight:bold;}"));
    t1->addWidget(aMinus);
    t1->addWidget(m_lblAccept);
    t1->addWidget(aPlus);
    t1->addSpacing(24);
    m_lblCoreThr = mkCap(w, QStringLiteral("core0 conf_threshold=--"));
    t1->addWidget(m_lblCoreThr);
    t1->addStretch(1);
    v->addLayout(t1);
    connect(tMinus, &QPushButton::clicked, this, &SettingsPage::onTriggerMinus);
    connect(tPlus, &QPushButton::clicked, this, &SettingsPage::onTriggerPlus);
    connect(aMinus, &QPushButton::clicked, this, &SettingsPage::onAcceptMinus);
    connect(aPlus, &QPushButton::clicked, this, &SettingsPage::onAcceptPlus);

    /* endpoint / model / key */
    QGridLayout *g = new QGridLayout();
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(6);
    g->addWidget(mkCap(w, QStringLiteral("API url")), 0, 0);
    m_edApiBase = new QLineEdit(w);
    m_edApiBase->setReadOnly(true);
    m_edApiBase->setMinimumHeight(42);
    g->addWidget(m_edApiBase, 0, 1, 1, 3);
    QPushButton *bApi = mkButton(QString::fromUtf8(TXT_EDIT), w);
    connect(bApi, &QPushButton::clicked, this, &SettingsPage::onEditApiBase);
    g->addWidget(bApi, 0, 4);

    g->addWidget(mkCap(w, QString::fromUtf8(TXT_MODEL)), 1, 0);
    m_edModel = new QLineEdit(w);
    m_edModel->setReadOnly(true);
    m_edModel->setMinimumHeight(42);
    g->addWidget(m_edModel, 1, 1, 1, 3);
    QPushButton *bModel = mkButton(QString::fromUtf8(TXT_EDIT), w);
    connect(bModel, &QPushButton::clicked, this, &SettingsPage::onEditModel);
    g->addWidget(bModel, 1, 4);
    QPushButton *bPreset = mkButton(QStringLiteral("preset"), w);
    connect(bPreset, &QPushButton::clicked, this, &SettingsPage::onModelPreset);
    g->addWidget(bPreset, 1, 5);

    g->addWidget(mkCap(w, QString::fromUtf8(TXT_KEY)), 2, 0);
    m_edKey = new QLineEdit(w);
    m_edKey->setReadOnly(true);
    m_edKey->setMinimumHeight(42);
    g->addWidget(m_edKey, 2, 1, 1, 3);
    QPushButton *bKey = mkButton(QString::fromUtf8(TXT_EDIT), w);
    connect(bKey, &QPushButton::clicked, this, &SettingsPage::onEditKey);
    g->addWidget(bKey, 2, 4);
    v->addLayout(g);

    /* toggles */
    QHBoxLayout *tog = new QHBoxLayout();
    m_ckAuto = new QCheckBox(QString::fromUtf8(TXT_AUTO), w);
    m_ckWriteback = new QCheckBox(QString::fromUtf8(TXT_WRITEBAK), w);
    m_ckInsecure = new QCheckBox(QString::fromUtf8(TXT_INSECURE), w);
    m_ckFake = new QCheckBox(QString::fromUtf8(TXT_FAKE), w);
    m_ckOutage = new QCheckBox(QString::fromUtf8(TXT_OUTAGE), w);
    QCheckBox *boxes[5] = { m_ckAuto, m_ckWriteback, m_ckInsecure, m_ckFake,
                            m_ckOutage };
    for (int i = 0; i < 5; ++i) {
        boxes[i]->setStyleSheet(QStringLiteral(
            "QCheckBox{color:#cfd8dc;font-size:17px;spacing:8px;}"));
        tog->addWidget(boxes[i]);
        connect(boxes[i], &QCheckBox::toggled, this,
                &SettingsPage::onToggleChanged);
    }
    tog->addStretch(1);
    v->addLayout(tog);

    /* actions */
    QHBoxLayout *act = new QHBoxLayout();
    QPushButton *bTest = mkButton(QString::fromUtf8(TXT_TEST), w, "#0d47a1");
    connect(bTest, &QPushButton::clicked, this, [this]() {
        setStatus(QStringLiteral("testing cloud ..."));
        m_cloud->testConnection();
    });
    QPushButton *bCheck = mkButton(
        QString::fromUtf8(TXT_CLOUD TXT_RECHECK), w, "#1b5e20");
    connect(bCheck, &QPushButton::clicked, this,
            [this]() { emit manualCloudRequested(); });
    QPushButton *bSave = mkButton(QString::fromUtf8(TXT_SAVE), w, "#33691e");
    connect(bSave, &QPushButton::clicked, this, &SettingsPage::onSave);
    act->addWidget(bTest);
    act->addWidget(bCheck);
    act->addWidget(bSave);
    act->addStretch(1);
    v->addLayout(act);

    m_lblCloudStat = mkCap(w, QStringLiteral("cloud: idle"));
    m_lblCloudStat->setWordWrap(true);
    v->addWidget(m_lblCloudStat);
    v->addStretch(1);
    return w;
}

/* ----------------------------------------------------------------- wifi */

QWidget *SettingsPage::buildWifiTab()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *v = new QVBoxLayout(w);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    m_lblWifi = mkVal(w, QStringLiteral("wlan0: --"));
    m_lblWifi->setStyleSheet(QStringLiteral("QLabel{color:#eceff1;"
                                            "font-size:19px;}"));
    v->addWidget(m_lblWifi);

    QGridLayout *g = new QGridLayout();
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(6);
    g->addWidget(mkCap(w, QStringLiteral("SSID")), 0, 0);
    m_edSsid = new QLineEdit(w);
    m_edSsid->setReadOnly(true);
    m_edSsid->setMinimumHeight(42);
    g->addWidget(m_edSsid, 0, 1, 1, 3);
    QPushButton *bSsid = mkButton(QString::fromUtf8(TXT_EDIT), w);
    connect(bSsid, &QPushButton::clicked, this, [this]() {
        const QString s = SoftKeyboard::getText(
            this, QStringLiteral("WiFi SSID"), m_edSsid->text(), false);
        if (!s.isNull())
            m_edSsid->setText(s);
    });
    g->addWidget(bSsid, 0, 4);

    g->addWidget(mkCap(w, QString::fromUtf8(TXT_PASS)), 1, 0);
    m_edPsk = new QLineEdit(w);
    m_edPsk->setReadOnly(true);
    m_edPsk->setMinimumHeight(42);
    g->addWidget(m_edPsk, 1, 1, 1, 3);
    QPushButton *bPsk = mkButton(QString::fromUtf8(TXT_EDIT), w);
    connect(bPsk, &QPushButton::clicked, this, [this]() {
        const QString s = SoftKeyboard::getText(
            this, QStringLiteral("WiFi passphrase"), m_edPsk->text(), true);
        if (!s.isNull())
            m_edPsk->setText(s);
    });
    g->addWidget(bPsk, 1, 4);
    v->addLayout(g);

    QHBoxLayout *act = new QHBoxLayout();
    m_btnConnect = mkButton(QString::fromUtf8(TXT_CONNECT), w, "#1b5e20");
    connect(m_btnConnect, &QPushButton::clicked, this, &SettingsPage::onConnect);
    QPushButton *bDisc = mkButton(QString::fromUtf8(TXT_DISCONN), w, "#4e342e");
    connect(bDisc, &QPushButton::clicked, this, [this]() {
        m_wifi->disconnectFrom();
    });
    QPushButton *bScan = mkButton(QString::fromUtf8(TXT_SCAN), w, "#0d47a1");
    connect(bScan, &QPushButton::clicked, this, [this]() {
        m_lstSsid->clear();
        m_wifi->scan();
    });
    QPushButton *bRestore = mkButton(QString::fromUtf8(TXT_RESTORE), w,
                                     "#33691e");
    connect(bRestore, &QPushButton::clicked, this,
            [this]() { m_wifi->restoreBackup(); });
    act->addWidget(m_btnConnect);
    act->addWidget(bDisc);
    act->addWidget(bScan);
    act->addWidget(bRestore);
    act->addStretch(1);
    v->addLayout(act);

    m_lstSsid = new QListWidget(w);
    m_lstSsid->setStyleSheet(QStringLiteral(
        "QListWidget{background:#101418;color:#eceff1;font-size:17px;"
        "border:1px solid #37474f;}"
        "QListWidget::item{height:34px;}"));
    connect(m_lstSsid, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *it) { m_edSsid->setText(it->text()); });
    v->addWidget(m_lstSsid, 1);

    m_lblWifiHint = mkCap(w, QString());
    m_lblWifiHint->setWordWrap(true);
    m_lblWifiHint->setText(QStringLiteral(
        "apply = ip link up -> wpa_supplicant -B -D nl80211 -> udhcpc; "
        "no lease within 20s restores %1 automatically")
                               .arg(WifiManager::backupPath()));
    v->addWidget(m_lblWifiHint);
    return w;
}

/* ----------------------------------------------------------- diagnostics */

QWidget *SettingsPage::buildDiagTab()
{
    QWidget *w = new QWidget(this);
    QVBoxLayout *v = new QVBoxLayout(w);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    QHBoxLayout *h = new QHBoxLayout();
    h->addWidget(mkCap(w, QString::fromUtf8(TXT_SERVICES)));
    h->addStretch(1);
    QPushButton *bRefresh = mkButton(QString::fromUtf8(TXT_REFRESH), w,
                                     "#0d47a1");
    connect(bRefresh, &QPushButton::clicked, this,
            &SettingsPage::refreshDiag);
    h->addWidget(bRefresh);
    v->addLayout(h);

    m_lblDiag = mkVal(w, QString());
    m_lblDiag->setStyleSheet(QStringLiteral("QLabel{color:#cfd8dc;"
                                            "font-size:16px;}"));
    v->addWidget(m_lblDiag);

    v->addWidget(mkCap(w, QString::fromUtf8(TXT_DEVICES)));
    m_lblDiagDev = mkVal(w, QString());
    m_lblDiagDev->setStyleSheet(QStringLiteral("QLabel{color:#cfd8dc;"
                                               "font-size:16px;}"));
    v->addWidget(m_lblDiagDev);
    v->addStretch(1);
    return w;
}

/* ------------------------------------------------------------- behaviour */

void SettingsPage::setStatus(const QString &line)
{
    if (m_lblStatus != nullptr)
        m_lblStatus->setText(line);
}

void SettingsPage::applyToUi()
{
    if (m_s == nullptr)
        return;
    m_lblTrigger->setText(QString::number(m_s->triggerConf, 'f', 2));
    m_lblAccept->setText(QString::number(m_s->acceptConf, 'f', 2));
    m_edApiBase->setText(m_s->apiBase);
    m_edModel->setText(m_s->model);
    m_edKey->setText(cloud_settings_mask_key(m_s->apiKey));
    /* Blocking the toggled() signals matters: without it every setChecked()
     * below would run onToggleChanged() (saving the file up to 5 times with
     * half-updated state pushed into the live CloudClient). */
    {
        QSignalBlocker b1(m_ckAuto);
        QSignalBlocker b2(m_ckWriteback);
        QSignalBlocker b3(m_ckInsecure);
        QSignalBlocker b4(m_ckFake);
        QSignalBlocker b5(m_ckOutage);
        m_ckAuto->setChecked(m_s->autoFallback);
        m_ckWriteback->setChecked(m_s->writeback);
        m_ckInsecure->setChecked(m_s->insecureTls);
        m_ckFake->setChecked(m_s->fakeResult);
        if (m_ckOutage != nullptr && m_cloud != nullptr)
            m_ckOutage->setChecked(m_cloud->outageSimulation());
    }
    onCloudStats();
}

void SettingsPage::refreshAll()
{
    applyToUi();
    if (m_wifi != nullptr) {
        m_wifi->refresh();
        onWifiStatus(m_wifi->status());
    }
    refreshDiag();
}

void SettingsPage::setCoreThreshold(double thr, bool valid)
{
    if (m_lblCoreThr == nullptr)
        return;
    m_lblCoreThr->setText(QStringLiteral("core0 conf_threshold=%1")
                              .arg(valid ? QString::number(thr, 'f', 2)
                                         : QStringLiteral("--")));
}

void SettingsPage::save()
{
    if (m_s == nullptr)
        return;
    if (m_cloud != nullptr)
        m_cloud->setSettings(*m_s);      /* live object follows the UI */
    if (cloud_settings_save(*m_s))
        setStatus(QStringLiteral("saved to %1").arg(cloud_settings_path()));
    else
        setStatus(QStringLiteral("SAVE FAILED (%1)")
                      .arg(cloud_settings_path()));
}

/* Re-read everything each time the page becomes visible. */
void SettingsPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refreshAll();
}

void SettingsPage::onTriggerMinus()
{
    m_s->triggerConf = qMax(0.05, m_s->triggerConf - 0.05);
    applyToUi();
    save();
}

void SettingsPage::onTriggerPlus()
{
    m_s->triggerConf = qMin(1.0, m_s->triggerConf + 0.05);
    applyToUi();
    save();
}

void SettingsPage::onAcceptMinus()
{
    m_s->acceptConf = qMax(0.0, m_s->acceptConf - 0.05);
    applyToUi();
    save();
}

void SettingsPage::onAcceptPlus()
{
    m_s->acceptConf = qMin(1.0, m_s->acceptConf + 0.05);
    applyToUi();
    save();
}

void SettingsPage::onEditApiBase()
{
    const QString s = SoftKeyboard::getText(
        this, QStringLiteral("Cloud API url"), m_s->apiBase, false);
    if (s.isNull() || s.trimmed().isEmpty())
        return;
    m_s->apiBase = s.trimmed();
    /* a hand-edited endpoint may point at another provider: adopt its key */
    const bool haveKey = cloud_settings_adopt_key(m_s);
    applyToUi();
    save();
    if (!haveKey)
        setStatus(QStringLiteral("no stored key for %1 - press KEY/EDIT")
                      .arg(QUrl(m_s->apiBase).host()));
}

void SettingsPage::onEditModel()
{
    const QString s = SoftKeyboard::getText(
        this, QStringLiteral("Cloud model name"), m_s->model, false);
    if (s.isNull() || s.trimmed().isEmpty())
        return;
    m_s->model = s.trimmed();
    applyToUi();
    save();
}

/* One tap = one consistent provider: model AND endpoint move together, because
 * a model name without its matching /chat/completions URL is a guaranteed 404.
 * Each provider has its own API key, so the stored key for the new provider is
 * adopted as well (otherwise the previous provider's key would be sent and the
 * server answers "incorrect api key"). */
void SettingsPage::onModelPreset()
{
    struct Preset { const char *model; const char *base; };
    static const Preset presets[] = {
        { "deepseek-chat",
          "https://api.deepseek.com/chat/completions" },
        { "deepseek-reasoner",
          "https://api.deepseek.com/chat/completions" },
        { "qwen-vl-max",
          "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions" },
        { "qwen-plus",
          "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions" },
        { "gpt-4o-mini",
          "https://api.openai.com/v1/chat/completions" },
    };
    static int idx = 0;
    idx = (idx + 1) % 5;
    m_s->model = QString::fromLatin1(presets[idx].model);
    m_s->apiBase = QString::fromLatin1(presets[idx].base);
    const bool haveKey = cloud_settings_adopt_key(m_s);
    applyToUi();
    save();
    setStatus(QStringLiteral("preset -> %1 @ %2  key=%3")
                  .arg(m_s->model, QUrl(m_s->apiBase).host(),
                       haveKey ? cloud_settings_mask_key(m_s->apiKey)
                               : QStringLiteral("NONE for this provider")));
}

void SettingsPage::onEditKey()
{
    const QUrl u(m_s->apiBase);
    const QString s = SoftKeyboard::getText(
        this, QStringLiteral("API key for %1 (stored in %2)")
                  .arg(u.host(), cloud_settings_path()),
        QString(), true);
    if (s.isNull())
        return;
    /* stores the key under this provider's id, so switching back and forth on
     * the LCD keeps every provider's own key */
    cloud_settings_set_key(m_s, s);
    applyToUi();
    save();
    setStatus(QStringLiteral("key for %1 set: %2")
                  .arg(cloud_provider_for_base(m_s->apiBase),
                       cloud_settings_mask_key(m_s->apiKey)));
}

void SettingsPage::onToggleChanged()
{
    m_s->autoFallback = m_ckAuto->isChecked();
    m_s->writeback = m_ckWriteback->isChecked();
    m_s->insecureTls = m_ckInsecure->isChecked();
    m_s->fakeResult = m_ckFake->isChecked();
    if (m_cloud != nullptr) {
        m_cloud->setOutageSimulation(m_ckOutage->isChecked());
        m_cloud->setSettings(*m_s);
    }
    save();
}

void SettingsPage::onSave()
{
    if (m_cloud != nullptr)
        m_cloud->setSettings(*m_s);
    save();
}

void SettingsPage::onCloudStats()
{
    if (m_cloud == nullptr || m_lblCloudStat == nullptr)
        return;
    const CloudStats st = m_cloud->stats();
    const int total = st.ok + st.fail + st.unreadable;
    const qint64 avg = total > 0 ? st.totalMs / total : 0;
    /* TLS state matters on this board: the rootfs ships no CA bundle, so
     * HTTPS only works with ca_file=, /etc/park/ca.pem or insecure_tls. */
    QString tls;
    if (m_s != nullptr && m_s->insecureTls) {
        tls = QStringLiteral("tls=INSECURE (verification off)");
    } else {
        const QString ca = CloudClient::caBundlePathFor(*m_s);
        tls = ca.isEmpty()
            ? QStringLiteral("tls=NO CA BUNDLE - https will fail")
            : QStringLiteral("tls=ca %1").arg(ca);
    }
    tls += QSslSocket::supportsSsl()
        ? QStringLiteral("  ssl=%1")
              .arg(QSslSocket::sslLibraryBuildVersionString())
        : QStringLiteral("  ssl=NONE (Qt built without OpenSSL?)");
    if (m_s != nullptr)
        tls += QStringLiteral("  proxy=%1  transport=%2")
                   .arg(CloudClient::proxyModeText(*m_s),
                        CloudClient::transportText(*m_s));
    m_lblCloudStat->setText(QStringLiteral(
        "cloud: state=%1  ok=%2 fail=%3 unreadable=%4  avg=%5ms  "
        "last='%6' %7  err=%8\n%9")
                                .arg(m_cloud->statusText())
                                .arg(st.ok)
                                .arg(st.fail)
                                .arg(st.unreadable)
                                .arg(avg)
                                .arg(st.lastPlate)
                                .arg(st.lastConf, 0, 'f', 2)
                                .arg(st.lastError.isEmpty()
                                         ? QStringLiteral("-")
                                         : st.lastError)
                                .arg(tls));
}

void SettingsPage::onUiMessage(const QString &line)
{
    setStatus(line);
}

void SettingsPage::onCloudFailed(const QString &reason, const QString &detail)
{
    setStatus(QStringLiteral("cloud FAILED: %1 (%2)").arg(reason, detail));
}

void SettingsPage::onCloudUnreadable(const QString &detail, qint64 ms)
{
    setStatus(QStringLiteral("cloud: %1 [%2 ms]").arg(detail).arg(ms));
}

void SettingsPage::onCloudFinished(const QString &plate, double conf,
                                   int status, qint64 ms, bool writeback)
{
    setStatus(QStringLiteral("cloud: '%1' conf=%2 http=%3 %4ms writeback=%5")
                  .arg(plate)
                  .arg(conf, 0, 'f', 2)
                  .arg(status)
                  .arg(ms)
                  .arg(writeback ? 1 : 0));
}

void SettingsPage::onConnect()
{
    if (m_wifi == nullptr)
        return;
    const QString ssid = m_edSsid->text().trimmed();
    const QString psk = m_edPsk->text();
    if (ssid.isEmpty()) {
        setStatus(QStringLiteral("ssid is empty"));
        return;
    }
    setStatus(QStringLiteral("connecting to '%1' ...").arg(ssid));
    m_wifi->connectTo(ssid, psk);
}

void SettingsPage::onWifiStatus(const WifiStatus &s)
{
    if (m_lblWifi == nullptr)
        return;
    /* no address / gateway here on purpose: this label is on a public LCD */
    m_lblWifi->setText(QStringLiteral(
        "%1: %2  ssid=%3  signal=%4")
                           .arg(s.iface)
                           .arg(s.state)
                           .arg(s.ssid.isEmpty() ? QStringLiteral("-") : s.ssid)
                           .arg(s.quality >= 0 ? QString::number(s.quality)
                                               : QStringLiteral("-")));
}

void SettingsPage::onWifiScan(const QStringList &ssids)
{
    if (m_lstSsid == nullptr)
        return;
    m_lstSsid->clear();
    for (int i = 0; i < ssids.size(); ++i)
        m_lstSsid->addItem(ssids.at(i));
    if (ssids.isEmpty())
        setStatus(QStringLiteral("scan: no network found"));
}

void SettingsPage::onWifiBusy(bool busy, const QString &what)
{
    if (m_btnConnect != nullptr)
        m_btnConnect->setEnabled(!busy);
    if (busy)
        setStatus(QStringLiteral("wifi busy: %1").arg(what));
}

void SettingsPage::onWifiResult(bool ok, const QString &detail)
{
    setStatus(QStringLiteral("wifi %1: %2")
                  .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED"))
                  .arg(detail));
    if (m_wifi != nullptr)
        onWifiStatus(m_wifi->status());
}

void SettingsPage::onWifiMessage(const QString &line)
{
    setStatus(line);
}

void SettingsPage::refreshDiag()
{
    static const char *units[] = { "park-clock", "board-power", "m4-load",
                                   "core0-bus", "park-ui" };
    /* this rootfs keeps tools in /sbin: do not rely on a systemd unit's PATH */
    QString systemctl = QStringLiteral("systemctl");
    static const char *const dirs[] = { "/bin/", "/usr/bin/", "/sbin/",
                                        "/usr/sbin/" };
    for (int i = 0; i < 4; ++i) {
        const QString c = QString::fromLatin1(dirs[i]) +
                          QStringLiteral("systemctl");
        if (QFile::exists(c)) {
            systemctl = c;
            break;
        }
    }
    QString txt;
    for (int i = 0; i < 5; ++i) {
        QProcess p;
        p.start(systemctl,
                QStringList() << QStringLiteral("is-active")
                              << QString::fromLatin1(units[i]));
        /* systemctl is-active normally answers in <50 ms; the short cap keeps
         * the page open responsive even if dbus/systemd is wedged */
        p.waitForFinished(300);
        QString st = QString::fromLocal8Bit(p.readAll()).trimmed();
        if (st.isEmpty())
            st = p.state() == QProcess::NotRunning ? QStringLiteral("?")
                                                   : QStringLiteral("timeout");
        txt += QStringLiteral("%1: %2    ").arg(QString::fromLatin1(units[i]),
                                                st);
        if (i == 2)
            txt += QStringLiteral("\n");
    }
    if (m_lblDiag != nullptr)
        m_lblDiag->setText(txt);

    static const char *nodes[] = { "/dev/ttyRPMSG0", "/dev/shm/park_shm",
                                   "/dev/ttyACM0" };
    QString dev;
    for (int i = 0; i < 3; ++i) {
        const QString n = QString::fromLatin1(nodes[i]);
        dev += QStringLiteral("%1: %2    ")
                   .arg(n, QFile::exists(n) ? QStringLiteral("present")
                                            : QStringLiteral("MISSING"));
    }
    if (m_lblDiagDev != nullptr)
        m_lblDiagDev->setText(dev);
}

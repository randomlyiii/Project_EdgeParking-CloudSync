#include "mainwindow.h"
#include "k210_link.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFont>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSizePolicy>

/* Chinese UI literals as UTF-8 escapes: keeps every source file pure ASCII
 * (board toolchain rule) while the on-screen text stays Chinese. */
#define TXT_SLOTS    "\xE8\xBD\xA6\xE4\xBD\x8D"                                   /* che wei       */
#define TXT_FREE     "\xE7\xA9\xBA\xE9\x97\xB2"                                   /* free          */
#define TXT_TOTAL    "\xE6\x80\xBB"                                               /* total         */
#define TXT_PLATE    "\xE6\x9C\x80\xE8\xBF\x91\xE8\xBD\xA6\xE7\x89\x8C"           /* last plate    */
#define TXT_SOURCE   "\xE6\x9D\xA5\xE6\xBA\x90"                                   /* source        */
#define TXT_EDGE     "\xE7\xAB\xAF\xE4\xBE\xA7"                                   /* edge          */
#define TXT_CLOUD    "\xE4\xBA\x91\xE5\x85\x9C\xE5\xBA\x95"                       /* cloud fallback */
#define TXT_CONF     "\xE7\xBD\xAE\xE4\xBF\xA1\xE5\xBA\xA6"                       /* confidence    */
#define TXT_GATE     "\xE9\x97\xB8"                                               /* gate          */
#define TXT_OPEN     "\xE5\xBC\x80"                                               /* open          */
#define TXT_CLOSE    "\xE5\x85\xB3"                                               /* closed        */
#define TXT_GATEOP   "\xE5\xBC\x80\xE9\x97\xB8"                                   /* gate open ev  */
#define TXT_RECOG    "\xE8\xAF\x86\xE5\x88\xAB\xE4\xB8\xAD..."                    /* recognizing   */
#define TXT_NOVIDEO  "\xE6\x97\xA0\xE9\xA2\x84\xE8\xA7\x88"                       /* no preview    */
#define TXT_ONLINE   "\xE5\x9C\xA8\xE7\xBA\xBF"                                   /* online        */
#define TXT_OFFLINE  "\xE7\xA6\xBB\xE7\xBA\xBF"                                   /* offline       */
#define TXT_FAULT    "\xE6\x95\x85\xE9\x9A\x9C"                                   /* fault         */
#define TXT_DEMO     "\xE6\xBC\x94\xE7\xA4\xBA"                                   /* demo          */
#define TXT_GATECL   "\xE5\x85\xB3\xE9\x97\xB8"                                   /* close gate    */
#define TXT_CLOUDBUSY "\xE5\x85\x9C\xE5\xBA\x95\xE4\xB8\xAD"                      /* fallback busy */
#define TXT_CLOUDCHK "\xE4\xBA\x91\xE7\xAB\xAF\xE5\xA4\x8D\xE6\xA3\x80"            /* cloud recheck */

static QLabel *statusChip(QWidget *parent)
{
    QLabel *l = new QLabel(parent);
    l->setStyleSheet("QLabel{color:#cfd8dc;font-size:14px;}");
    /* A QLabel's minimumSizeHint is its full text width: one long line (a cloud
     * error detail, a long SSID) would then force the whole window wider than
     * the 1024 px panel and push the right-hand panel off-screen. Preferred
     * keeps the natural width while allowing a shrink, and an explicit
     * minimumWidth(0) overrides the text-derived minimum. (Do NOT use
     * QSizePolicy::Ignored here: "Ignored" means *greedy* - the chips then
     * collapse to zero width and the status bar looks empty. Measured on the
     * board 2026-09-11.) */
    l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    l->setMinimumWidth(0);
    return l;
}

/* Elide to a pixel budget so a chip cannot dominate the status bar. */
static QString elide(const QLabel *l, const QString &text, int px)
{
    if (l == nullptr)
        return text;
    const QFontMetrics fm(l->font());
    return fm.elidedText(text, Qt::ElideRight, px);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();

    connect(&m_frameTimer, &QTimer::timeout, this, &MainWindow::onFrameTick);
    connect(&m_clockTimer, &QTimer::timeout, this, &MainWindow::onClock);
    connect(&m_tickerTimer, &QTimer::timeout, this, &MainWindow::onTicker);
    connect(&m_popTimer, &QTimer::timeout, m_popup, &QWidget::hide);

    m_frameTimer.start(100);
    m_clockTimer.start(1000);
    m_tickerTimer.start(4000);
    onClock();
    applyCloudChip();        /* initial CLOUD:-- until step-7 cloud reports */

    /* Board (linuxfb, no window manager): showFullScreen() alone does NOT
     * resize the window here - measured on the board, the window stayed at
     * its sizeHint (384x302) and painted only the top-left quarter of the
     * 1024x600 panel. Set the geometry explicitly from the screen the driver
     * reports (FBIOGET_VSCREENINFO on this board: 1024x600, 16bpp, stride
     * 2048 - correct, so QScreen geometry is trustworthy here). */
    const QString plat = qgetenv("QT_QPA_PLATFORM").toLower();
    QScreen *scr = QGuiApplication::primaryScreen();
    QRect sg = scr ? scr->geometry() : QRect();
    if (sg.isEmpty())
        sg = QRect(0, 0, 1024, 600);
    if (plat.contains("linuxfb") || plat.contains("eglfs")) {
        /* no window manager: an explicit geometry is the whole story.
         * showFullScreen() is deliberately NOT used - the fullscreen state
         * left the window at its sizeHint here (see note above).
         * The maximum pins the window to the panel: Qt would otherwise grow it
         * to the layout's minimumSize when a long text lands in a label, and
         * everything past x=1024 (the whole right-hand panel) is unreadable on
         * this display because there is no window manager to scroll it back. */
        setMinimumSize(320, 200);
        setMaximumSize(sg.size());
        setGeometry(sg);
    } else {
        resize(1024, 600);
    }
    qWarning("display: plat='%s' screen=%dx%d+%d+%d dpr=%.2f -> window=%dx%d",
             qPrintable(plat), sg.width(), sg.height(), sg.x(), sg.y(),
             scr ? scr->devicePixelRatio() : 1.0, width(), height());
}

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    static bool logged = false;
    if (!logged) {
        logged = true;
        qWarning("display: visible window=%dx%d+%d+%d state=0x%x",
                 width(), height(), x(), y(), unsigned(windowState()));
    }
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    /* pluggable gate-control input source (core1 spec 5.6.1.3): O/C hotkeys
     * here, the two on-screen buttons below, and external tools writing the
     * shm pulse field directly. Every source goes through gateRequested() and
     * the same req_gate_open/close pulse semantics; IpcWriter logs the write
     * so the action is auditable regardless of the source. */
    if (e->key() == Qt::Key_O) {
        emit gateRequested(true);
    } else if (e->key() == Qt::Key_C) {
        emit gateRequested(false);
    } else {
        QMainWindow::keyPressEvent(e);
    }
}

void MainWindow::buildUi()
{
    QWidget *central = new QWidget(this);
    central->setStyleSheet("QWidget#central{background:#101418;}");
    central->setObjectName("central");
    setCentralWidget(central);

    QVBoxLayout *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    /* ---------- status bar (32 px) ---------- */
    QWidget *statusBar = new QWidget(central);
    statusBar->setFixedHeight(32);
    statusBar->setObjectName("statusBar");
    statusBar->setStyleSheet("QWidget#statusBar{background:#1c232b;}");
    QHBoxLayout *sb = new QHBoxLayout(statusBar);
    sb->setContentsMargins(12, 0, 12, 0);
    sb->setSpacing(18);

    m_lblSys = statusChip(statusBar);
    m_lblRpmsg = statusChip(statusBar);
    m_lblM4 = statusChip(statusBar);
    m_lblCore1 = statusChip(statusBar);
    m_lblWifi = statusChip(statusBar);      /* step 7 */
    m_lblCloud = statusChip(statusBar);
    m_lblClock = statusChip(statusBar);
    sb->addWidget(m_lblSys);
    sb->addWidget(m_lblRpmsg);
    sb->addWidget(m_lblM4);
    sb->addWidget(m_lblCore1);
    sb->addWidget(m_lblWifi);
    sb->addWidget(m_lblCloud);
    sb->addStretch(1);
    /* step 7: gear -> full-screen settings page (cloud / wifi / diagnostics) */
    m_btnSettings = new QPushButton(QStringLiteral("SET"), statusBar);
    m_btnSettings->setFixedSize(56, 26);
    m_btnSettings->setFocusPolicy(Qt::NoFocus);
    m_btnSettings->setStyleSheet(QStringLiteral(
        "QPushButton{background:#37474f;color:#eceff1;font-size:14px;"
        "border:1px solid #546e7a;border-radius:6px;}"
        "QPushButton:pressed{background:#546e7a;}"));
    connect(m_btnSettings, &QPushButton::clicked, this, &MainWindow::openSettings);
    sb->addWidget(m_btnSettings);
    sb->addWidget(m_lblClock);
    root->addWidget(statusBar);

    /* ---------- middle row: preview + right panel ---------- */
    QHBoxLayout *mid = new QHBoxLayout();
    mid->setContentsMargins(8, 8, 8, 8);
    mid->setSpacing(8);
    root->addLayout(mid, 1);

    m_previewBox = new QWidget(central);
    m_previewBox->setObjectName("previewBox");
    m_previewBox->setStyleSheet("QWidget#previewBox{background:#000000;}");
    QVBoxLayout *pv = new QVBoxLayout(m_previewBox);
    pv->setContentsMargins(0, 0, 0, 0);
    m_lblPreview = new QLabel(m_previewBox);
    m_lblPreview->setAlignment(Qt::AlignCenter);
    m_lblPreview->setStyleSheet("QLabel{color:#546e7a;font-size:20px;}");
    m_lblPreview->setText(TXT_NOVIDEO);
    /* the preview must be able to shrink: its pixmap size would otherwise feed
     * back into the layout minimum */
    m_lblPreview->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_lblPreview->setMinimumSize(0, 0);
    pv->addWidget(m_lblPreview);

    m_lblBadge = new QLabel(m_previewBox);          /* top-right badge */
    m_lblBadge->setStyleSheet("QLabel{background:#ff8f00;color:#101418;"
                              "font-size:14px;padding:4px 10px;border-radius:6px;}");
    m_lblBadge->setText(TXT_RECOG);
    m_lblBadge->adjustSize();
    m_lblBadge->hide();
    mid->addWidget(m_previewBox, 1);

    QWidget *panel = new QWidget(central);
    panel->setFixedWidth(300);
    QVBoxLayout *pl = new QVBoxLayout(panel);
    pl->setContentsMargins(4, 4, 4, 4);
    pl->setSpacing(6);

    QLabel *capSlots = new QLabel(QString(TXT_SLOTS), panel);
    capSlots->setStyleSheet("QLabel{color:#90a4ae;font-size:16px;}");
    m_lblSlots = new QLabel(panel);
    m_lblSlots->setStyleSheet("QLabel{color:#eceff1;font-size:34px;font-weight:bold;}");
    pl->addWidget(capSlots);
    pl->addWidget(m_lblSlots);
    pl->addSpacing(12);

    QLabel *capPlate = new QLabel(QString(TXT_PLATE), panel);
    capPlate->setStyleSheet("QLabel{color:#90a4ae;font-size:16px;}");
    m_lblPlate = new QLabel("--", panel);
    m_lblPlate->setStyleSheet("QLabel{color:#eceff1;font-size:26px;font-weight:bold;}");
    m_lblPlate->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_lblPlate->setMinimumWidth(0);
    m_lblSource = new QLabel(panel);
    m_lblSource->setStyleSheet("QLabel{color:#b0bec5;font-size:15px;}");
    m_lblSource->setMinimumWidth(0);
    m_lblConf = new QLabel(panel);
    m_lblConf->setStyleSheet("QLabel{color:#b0bec5;font-size:15px;}");
    m_lblConf->setMinimumWidth(0);
    pl->addWidget(capPlate);
    pl->addWidget(m_lblPlate);
    pl->addWidget(m_lblSource);
    pl->addWidget(m_lblConf);
    pl->addStretch(1);
    mid->addWidget(panel);

    /* ---------- bottom bar (40 px) ---------- */
    QWidget *bottom = new QWidget(central);
    bottom->setFixedHeight(40);
    bottom->setObjectName("bottomBar");
    bottom->setStyleSheet("QWidget#bottomBar{background:#1c232b;}");
    QHBoxLayout *bb = new QHBoxLayout(bottom);
    bb->setContentsMargins(12, 0, 12, 0);
    bb->setSpacing(18);
    m_lblGate = new QLabel(bottom);
    m_lblGate->setStyleSheet("QLabel{color:#cfd8dc;font-size:15px;}");
    m_lblEvents = new QLabel(bottom);
    m_lblEvents->setStyleSheet("QLabel{color:#90a4ae;font-size:14px;}");
    /* the event ticker carries the longest strings on screen (cloud failures,
     * K210 errors): it must clip, never push the buttons off the panel */
    m_lblEvents->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_lblEvents->setMinimumWidth(0);
    m_lblGate->setMinimumWidth(0);

    /* operator trigger source (core1 spec 5.6.1.3): the board has no keyboard,
     * so the gate buttons are the primary source; touch arrives as a mouse
     * click through libinput. They only request - Core0 decides and executes. */
    m_btnGateOpen = new QPushButton(QString(TXT_GATEOP), bottom);
    m_btnGateOpen->setFixedSize(96, 32);
    m_btnGateOpen->setFocusPolicy(Qt::NoFocus);
    m_btnGateOpen->setStyleSheet(
        "QPushButton{background:#1b5e20;color:#e8f5e9;font-size:16px;"
        "border:1px solid #2e7d32;border-radius:6px;}"
        "QPushButton:pressed{background:#2e7d32;}");
    m_btnGateClose = new QPushButton(QString(TXT_GATECL), bottom);
    m_btnGateClose->setFixedSize(96, 32);
    m_btnGateClose->setFocusPolicy(Qt::NoFocus);
    m_btnGateClose->setStyleSheet(
        "QPushButton{background:#4e342e;color:#efebe9;font-size:16px;"
        "border:1px solid #6d4c41;border-radius:6px;}"
        "QPushButton:pressed{background:#6d4c41;}");
    /* step 7: ask the cloud about the current frame right now */
    m_btnCloudCheck = new QPushButton(QString::fromUtf8(TXT_CLOUDCHK), bottom);
    m_btnCloudCheck->setFixedSize(150, 32);
    m_btnCloudCheck->setFocusPolicy(Qt::NoFocus);
    m_btnCloudCheck->setStyleSheet(
        "QPushButton{background:#0d47a1;color:#e3f2fd;font-size:16px;"
        "border:1px solid #1565c0;border-radius:6px;}"
        "QPushButton:pressed{background:#1565c0;}");
    connect(m_btnGateOpen, &QPushButton::clicked, this,
            [this]() { emit gateRequested(true); });
    connect(m_btnGateClose, &QPushButton::clicked, this,
            [this]() { emit gateRequested(false); });
    connect(m_btnCloudCheck, &QPushButton::clicked, this,
            [this]() { emit cloudCheckRequested(); });

    bb->addWidget(m_lblGate);
    bb->addStretch(1);
    bb->addWidget(m_lblEvents, 1);
    bb->addWidget(m_btnCloudCheck);
    bb->addWidget(m_btnGateOpen);
    bb->addWidget(m_btnGateClose);
    root->addWidget(bottom);

    /* ---------- popup card (child of preview box, centered) ---------- */
    m_popup = new QWidget(m_previewBox);
    m_popup->setObjectName("platePopup");
    m_popup->setStyleSheet("QWidget#platePopup{background:#1b2a33;"
                           "border:3px solid #00e676;border-radius:10px;}");
    QVBoxLayout *pp = new QVBoxLayout(m_popup);
    pp->setContentsMargins(18, 10, 18, 10);
    m_popPlate = new QLabel(m_popup);
    m_popPlate->setStyleSheet("QLabel{color:#eceff1;font-size:30px;font-weight:bold;"
                              "background:transparent;border:none;}");
    m_popPlate->setAlignment(Qt::AlignCenter);
    m_popSub = new QLabel(m_popup);
    m_popSub->setStyleSheet("QLabel{color:#b0bec5;font-size:15px;"
                            "background:transparent;border:none;}");
    m_popSub->setAlignment(Qt::AlignCenter);
    pp->addWidget(m_popPlate);
    pp->addWidget(m_popSub);
    m_popup->setFixedSize(360, 120);
    m_popup->hide();
}

/* ------------------------------ events ------------------------------ */

void MainWindow::pushEvent(const QString &line)
{
    /* keep the ticker line bounded: cloud failure details can be long and the
     * bottom bar must never grow past the 1024 px panel (panel is 300 px) */
    QString text = line;
    if (text.size() > 90)
        text = text.left(87) + QStringLiteral("...");
    const QString stamp =
        QDateTime::currentDateTime().toString("HH:mm:ss") + " " + text;
    m_events.append(stamp);
    if (m_events.size() > 64)
        m_events.remove(0, m_events.size() - 64);
    m_evtCursor = m_events.size() - 1;          /* newest first */
    m_lblEvents->setText(elide(m_lblEvents, m_events.last(), 470));
    m_lblEvents->setToolTip(stamp);
}

/* ------------------------------ popup ------------------------------ */

void MainWindow::showPlatePopup(const QString &plate, double confidence,
                                int source, bool deny)
{
    const QString src = source == 1 ? QString(TXT_CLOUD) : QString(TXT_EDGE);
    m_popPlate->setText(plate);
    m_popSub->setText(QString("%1  conf=%2")
                      .arg(src).arg(confidence, 0, 'f', 2));
    m_popup->setStyleSheet(deny
        ? "QWidget#platePopup{background:#331b1e;border:3px solid #ff1744;"
          "border-radius:10px;}"
        : "QWidget#platePopup{background:#1b2a33;border:3px solid #00e676;"
          "border-radius:10px;}");
    /* center over the preview box */
    m_popup->move((m_previewBox->width() - m_popup->width()) / 2,
                  (m_previewBox->height() - m_popup->height()) / 2);
    m_popup->raise();
    m_popup->show();
    m_popTimer.start(3000);                     /* 3 s auto-hide per spec */

    m_lblPlate->setText(plate);
    m_lblSource->setText(QString(TXT_SOURCE ": %1").arg(src));
    m_lblConf->setText(QString(TXT_CONF ": %1").arg(confidence, 0, 'f', 2));
}

/* ------------------------------ slots ------------------------------ */

void MainWindow::onFrameTick()
{
    if (!m_link)
        return;
    QImage frame;
    if (m_link->takeFrame(&frame) && !frame.isNull()) {
        m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
        m_lblPreview->setPixmap(QPixmap::fromImage(frame).scaled(
            m_lblPreview->size(), Qt::KeepAspectRatio,
            Qt::FastTransformation));           /* soft board: fast scale */
    }
    const bool stale =
        m_lastFrameMs == 0 ||
        QDateTime::currentMSecsSinceEpoch() - m_lastFrameMs > 2000;
    if (stale && !m_lblPreview->pixmap() && m_lblPreview->text().isEmpty())
        m_lblPreview->setText(TXT_NOVIDEO);
}

void MainWindow::onClock()
{
    m_lblClock->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));
}

void MainWindow::onTicker()
{
    if (m_events.isEmpty())
        return;
    /* rotate through the last few events so the single line stays alive */
    const int start = qMax(0, m_events.size() - 5);
    if (m_events.size() - start > 1)
        m_evtCursor = start + (m_evtCursor + 1 - start) % (m_events.size() - start);
    m_lblEvents->setText(m_events.at(m_evtCursor));
}

void MainWindow::onSnapshot(const IpcSnapshot &snap)
{
    applySnapshot(snap);
}

void MainWindow::onLinkUp(bool up)
{
    pushEvent(QString("k210 link %1").arg(up ? "UP" : "DOWN"));
    if (!up) {                             /* link dropped: clear busy (D4) */
        m_k210Busy = false;
        updateBadge();
    }
}

void MainWindow::onRecogResult(const QString &plate, double confidence,
                               int source)
{
    showPlatePopup(plate, confidence, source, false);
    pushEvent(QString("recog: %1 conf=%2 (%3)")
              .arg(plate).arg(confidence, 0, 'f', 2)
              .arg(source == 1 ? "cloud" : "edge"));
}

void MainWindow::onRecogFailed(const QString &reason)
{
    pushEvent(QString("recog failed: %1").arg(reason));
}

void MainWindow::onCloudPending(bool pending)
{
    /* write-end feedback only: the chip itself follows the shm field (single
     * source of truth, core1 spec 5.8.3), which the reader polls <=200ms */
    pushEvent(pending ? QStringLiteral("cloud fallback in progress")
                      : QStringLiteral("cloud fallback done"));
    applyCloudChip();
}

/* ------------------------- step 7: status chips ------------------------- */

void MainWindow::setWifiChip(const QString &text, bool ok)
{
    if (m_lblWifi == nullptr)
        return;
    /* "WIFI:" + ssid can get long: keep it inside ~170 px */
    m_lblWifi->setText(QStringLiteral("WIFI:") + elide(m_lblWifi, text, 150));
    m_lblWifi->setStyleSheet(ok ? "QLabel{color:#69f0ae;font-size:14px;}"
                                : "QLabel{color:#ff5252;font-size:14px;}");
}

void MainWindow::setCloudChip(const QString &text, bool ok)
{
    m_cloudHealth = text;
    m_cloudHealthOk = ok;
    applyCloudChip();
}

/* ------------------------ step 7: settings page ------------------------ */

void MainWindow::setSettingsPage(QWidget *page)
{
    m_settingsPage = page;
    if (m_settingsPage == nullptr)
        return;
    m_settingsPage->setParent(this);
    m_settingsPage->setGeometry(rect());
    m_settingsPage->hide();
    connect(m_settingsPage, SIGNAL(closed()), this, SLOT(closeSettings()));
}

void MainWindow::openSettings()
{
    if (m_settingsPage == nullptr)
        return;
    m_settingsPage->setGeometry(rect());
    m_settingsPage->show();
    m_settingsPage->raise();
    pushEvent(QStringLiteral("settings page opened"));
}

void MainWindow::closeSettings()
{
    if (m_settingsPage == nullptr)
        return;
    m_settingsPage->hide();
    pushEvent(QStringLiteral("settings page closed"));
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    if (m_settingsPage != nullptr && m_settingsPage->isVisible())
        m_settingsPage->setGeometry(rect());
}

/* --------------------------- snapshot apply --------------------------- */

static void setChip(QLabel *chip, const QString &name, bool ok,
                    const char *onTxt, const char *offTxt)
{
    const QString state = name + ": " + (ok ? QString::fromUtf8(onTxt)
                                            : QString::fromUtf8(offTxt));
    chip->setText(state);
    chip->setStyleSheet(ok ? "QLabel{color:#69f0ae;font-size:14px;}"
                           : "QLabel{color:#ff5252;font-size:14px;}");
}

void MainWindow::applySnapshot(const IpcSnapshot &s)
{
    /* status bar */
    m_lblSys->setText(s.demo ? QString("SYS:" TXT_DEMO) : QString("SYS:OK"));
    const bool rpmsg = s.shmOnline ? ((s.linkFlags & 0x01) != 0) : false;
    const bool m4 = s.shmOnline ? ((s.linkFlags & 0x02) != 0) : false;
    const bool core1 = s.shmOnline ? ((s.linkFlags & 0x04) != 0) : true;
    if (s.demo) {
        setChip(m_lblRpmsg, "RPMSG", true, TXT_ONLINE, TXT_OFFLINE);
        setChip(m_lblM4, "M4", true, TXT_ONLINE, TXT_OFFLINE);
        setChip(m_lblCore1, "CORE1", true, TXT_ONLINE, TXT_OFFLINE);
    } else {
        setChip(m_lblRpmsg, "RPMSG", rpmsg, TXT_ONLINE, TXT_OFFLINE);
        setChip(m_lblM4, "M4", m4, TXT_ONLINE, TXT_OFFLINE);
        setChip(m_lblCore1, "CORE1", core1, TXT_ONLINE, TXT_OFFLINE);
    }
    applyCloudChip();
    /* step 7: settings page shows Core0's authoritative threshold read-only */
    emit coreThresholdChanged(s.confThreshold, s.shmOnline);

    /* slots */
    m_lblSlots->setText(QString(TXT_FREE " %1 / " TXT_TOTAL " %2")
                        .arg(s.freeSlots).arg(s.freeSlots + s.usedSlots));
    m_lblSlots->setStyleSheet(s.freeSlots < 3
        ? "QLabel{color:#ffab40;font-size:34px;font-weight:bold;}"   /* orange */
        : "QLabel{color:#eceff1;font-size:34px;font-weight:bold;}");

    /* gate */
    m_lblGate->setText(QString(TXT_GATE ": ") + (s.gateOpen ? TXT_OPEN : TXT_CLOSE));
    m_lblGate->setStyleSheet(s.gateOpen
        ? "QLabel{color:#69f0ae;font-size:15px;}"
        : "QLabel{color:#cfd8dc;font-size:15px;}");

    /* recognition badge is driven by the merged state machine (D4) */
    updateBadge();

    /* persistent plate when no fresher popup content */
    if (!s.plate.isEmpty() && m_lblPlate->text() == "--") {
        m_lblPlate->setText(s.plate);
        m_lblSource->setText(QString(TXT_SOURCE ": ") +
                             (s.source == 1 ? TXT_CLOUD : TXT_EDGE));
        m_lblConf->setText(QString(TXT_CONF ": %1").arg(s.confidence, 0, 'f', 2));
    }

    /* IPC writer state events */
    static bool firstRun = true;
    static bool lastOnline = false;
    if (firstRun) {
        firstRun = false;
        lastOnline = s.shmOnline;
        if (s.shmOnline)
            pushEvent("ipc: /park_shm attached");
    } else if (s.shmOnline != lastOnline) {
        lastOnline = s.shmOnline;
        pushEvent(s.shmOnline ? "ipc: /park_shm attached"
                              : "ipc: /park_shm lost");
    }
}

/* --------------------------- badge merge --------------------------- */

void MainWindow::onK210Busy(bool busy)
{
    m_k210Busy = busy;
    updateBadge();
}

void MainWindow::updateBadge()
{
    const bool pending = m_ipc ? m_ipc->snapshot().recogPending : false;
    const bool badge = pending || m_k210Busy;        /* D4: dual-source merge */
    m_lblBadge->setVisible(badge);
    if (badge)
        m_lblBadge->move(m_previewBox->width() - m_lblBadge->width() - 12, 12);
}

/* --------------------------- cloud chip --------------------------- */

void MainWindow::applyCloudChip()
{
    const bool demo = m_ipc ? m_ipc->snapshot().demo : false;
    if (demo) {
        m_lblCloud->setText("CLOUD:OK");
        m_lblCloud->setStyleSheet("QLabel{color:#69f0ae;font-size:14px;}");
        return;
    }
    /* step 6: cloud fallback in flight (Core1 write-end raises cloud_pending,
     * Core0 mirrors it in shm). The shm field is the single source of truth. */
    const IpcSnapshot snap = m_ipc ? m_ipc->snapshot() : IpcSnapshot();
    if (snap.shmOnline && snap.cloudPending) {
        m_lblCloud->setText(QString("CLOUD:") +
                            QString::fromUtf8(TXT_CLOUDBUSY));
        m_lblCloud->setStyleSheet("QLabel{color:#ffab40;font-size:14px;}");
        return;
    }
    /* step 7: cloud-client health (idle / ok / no key / outage ...) */
    if (!m_cloudHealth.isEmpty()) {
        m_lblCloud->setText(QStringLiteral("CLOUD:") +
                            elide(m_lblCloud, m_cloudHealth, 170));
        m_lblCloud->setStyleSheet(m_cloudHealthOk
                                      ? "QLabel{color:#69f0ae;font-size:14px;}"
                                      : "QLabel{color:#ffab40;font-size:14px;}");
        return;
    }
    switch (m_cloudState) {
    case Online:
        m_lblCloud->setText(QString("CLOUD:") + QString::fromUtf8(TXT_ONLINE));
        m_lblCloud->setStyleSheet("QLabel{color:#69f0ae;font-size:14px;}");
        break;
    case Offline:
        m_lblCloud->setText(QString("CLOUD:") + QString::fromUtf8(TXT_OFFLINE));
        m_lblCloud->setStyleSheet("QLabel{color:#ff5252;font-size:14px;}");
        break;
    default:                                         /* Unknown */
        m_lblCloud->setText("CLOUD:--");
        m_lblCloud->setStyleSheet("QLabel{color:#90a4ae;font-size:14px;}");
        break;
    }
}

void MainWindow::setCloudState(CloudState state)
{
    m_cloudState = state;
    applyCloudChip();
}

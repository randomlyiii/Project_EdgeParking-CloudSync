#ifndef MAINWINDOW_H
#define MAINWINDOW_H
/* MainWindow - parking lot UI per PhaseMd/11 (1024x600 baseline).
 *
 * Layout:
 *   [status bar 32px: SYS | RPMSG | M4 | CORE1 | CLOUD | clock]
 *   [preview video area            | slots big number        ]
 *   [(JPEG soft-decode, badge)     | last plate/source/conf  ]
 *   [bottom bar 40px: gate state | event ticker              ]
 * Plate popup card: 3 s auto-hide, red on DENY.
 *
 * Data paths (spec section 2): frames pulled at 100 ms (drop-old, always
 * newest), IPC snapshot event/poll-driven, clock 1 s, ticker 4 s.
 */
#include <QMainWindow>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QShowEvent>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include "ipc_reader.h"

class K210Link;
class QPushButton;

enum CloudState { Unknown, Online, Offline };

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void setLink(K210Link *link) { m_link = link; }
    void setIpc(IpcReader *ipc) { m_ipc = ipc; }

signals:
    void gateRequested(bool open);   /* open=true open gate, false close */
    void cloudCheckRequested();      /* operator asked for a cloud re-check */
    void coreThresholdChanged(double thr, bool valid); /* -> settings page */

public slots:
    void pushEvent(const QString &line);
    void showPlatePopup(const QString &plate, double confidence, int source,
                        bool deny);
    void setCloudState(CloudState state);
    /* connected from main.cpp via pointer-to-member, must be public */
    void onSnapshot(const IpcSnapshot &snap);
    void onLinkUp(bool up);
    void onRecogResult(const QString &plate, double confidence, int source);
    void onRecogFailed(const QString &reason);
    void onK210Busy(bool busy);
    void onCloudPending(bool pending);
    /* step 7 status chips: wifi link and cloud health */
    void setWifiChip(const QString &text, bool ok);
    void setCloudChip(const QString &text, bool ok);
    /* step 7: full-screen operator page (owned by main.cpp) */
    void setSettingsPage(QWidget *page);
    void openSettings();
    void closeSettings();

protected:
    void showEvent(QShowEvent *e) override;   /* logs the real window size */
    void keyPressEvent(QKeyEvent *e) override; /* gate open/close hotkeys */
    void resizeEvent(QResizeEvent *e) override; /* keep the page full-screen */

private slots:
    void onFrameTick();     /* 100 ms: pull newest decoded frame */
    void onClock();         /* 1 s */
    void onTicker();        /* 4 s: rotate recent events */

private:
    void buildUi();
    void applySnapshot(const IpcSnapshot &s);
    void updateBadge();
    void applyCloudChip();

    /* --- data providers --- */
    K210Link *m_link = nullptr;
    IpcReader *m_ipc = nullptr;

    /* --- status bar --- */
    QLabel *m_lblSys = nullptr;
    QLabel *m_lblRpmsg = nullptr;
    QLabel *m_lblM4 = nullptr;
    QLabel *m_lblCore1 = nullptr;
    QLabel *m_lblCloud = nullptr;
    QLabel *m_lblWifi = nullptr;        /* step 7: wlan0 link state */
    QLabel *m_lblClock = nullptr;
    CloudState m_cloudState = Unknown;
    QString m_cloudHealth;              /* step 7: cloud-client health text */
    bool m_cloudHealthOk = false;

    /* --- preview --- */
    QWidget *m_previewBox = nullptr;
    QLabel *m_lblPreview = nullptr;
    QLabel *m_lblBadge = nullptr;
    qint64 m_lastFrameMs = 0;
    bool m_k210Busy = false;

    /* --- right panel --- */
    QLabel *m_lblSlots = nullptr;      /* big free/total number */
    QLabel *m_lblPlate = nullptr;      /* persistent last plate */
    QLabel *m_lblSource = nullptr;
    QLabel *m_lblConf = nullptr;

    /* --- bottom bar --- */
    QLabel *m_lblGate = nullptr;
    QLabel *m_lblEvents = nullptr;
    /* operator gate trigger (spec 5.6): buttons emit gateRequested() */
    QPushButton *m_btnGateOpen = nullptr;
    QPushButton *m_btnGateClose = nullptr;
    QPushButton *m_btnCloudCheck = nullptr;   /* step 7: manual cloud recheck */
    QPushButton *m_btnSettings = nullptr;     /* step 7: gear -> settings page */
    QWidget *m_settingsPage = nullptr;        /* owned by main.cpp */

    /* --- popup card --- */
    QWidget *m_popup = nullptr;
    QLabel *m_popPlate = nullptr;
    QLabel *m_popSub = nullptr;
    QTimer m_popTimer;

    /* --- timers --- */
    QTimer m_frameTimer;
    QTimer m_clockTimer;
    QTimer m_tickerTimer;

    /* --- events --- */
    QVector<QString> m_events;
    int m_evtCursor = 0;
};

#endif /* MAINWINDOW_H */

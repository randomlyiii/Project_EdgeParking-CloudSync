#ifndef IPC_READER_H
#define IPC_READER_H
/* IpcReader - snapshot provider for the UI.
 *
 * Real mode : attaches shm "/park_shm" (writer = core0_service, P6-01) and
 *             re-publishes a snapshot on change. Refresh paths per PhaseMd/11:
 *             eventfd-driven when an fd is passed (--eventfd, wired by P6-02),
 *             otherwise 200 ms polling (within the 500 ms KPI).
 * Demo mode : when the shm does not exist yet (Core0 business not built), a
 *             built-in simulator feeds the UI so Q-02..Q-05 can be developed
 *             and reviewed standalone. Enabled with --demo on, or auto
 *             (default) when the shm is absent.
 */
#include <QObject>
#include <QTimer>
#include <QSocketNotifier>

#include "park_shm.h"

struct IpcSnapshot
{
    bool demo = false;          /* true = simulated data */
    bool shmOnline = false;     /* shm attached */
    int freeSlots = 0;
    int usedSlots = 0;
    bool gateOpen = false;
    bool recogPending = false;
    bool cloudPending = false;  /* core1 write-end: cloud fallback in flight */
    quint8 linkFlags = 0;       /* bit0 rpmsg bit1 m4 bit2 core1 */
    QString plate;              /* persistent last plate (may be empty) */
    double confidence = 0.0;
    int source = 0;             /* 0=edge 1=cloud */
};

class IpcReader : public QObject
{
    Q_OBJECT
public:
    enum DemoMode { DemoAuto, DemoForce, DemoOff };

    explicit IpcReader(QObject *parent = nullptr);
    ~IpcReader() override;

    void start(DemoMode demoMode, int eventfdFd);
    void stop();

    IpcSnapshot snapshot() const { return m_snap; }

signals:
    void snapshotChanged(const IpcSnapshot &snap);
    void eventMessage(const QString &msg);
    /* demo only: simulated recognition popups (real ones come from K210Link) */
    void platePopup(const QString &plate, double confidence, int source,
                    bool deny);

public slots:
    /* refresh now (wired from IpcWriter's snapshotRefreshRequested) */
    void onTick();

private slots:
    void onEventfd();

private:
    bool attachShm();
    bool readShm(IpcSnapshot *out);
    void demoTick();

    QTimer m_timer;
    QTimer m_demoTimer;
    QSocketNotifier *m_evtNotifier = nullptr;
    int m_eventfd = -1;

    park_shm_t *m_shm = nullptr;   /* mapped, null until attach succeeds */
    bool m_demo = false;
    IpcSnapshot m_snap;

    /* demo state */
    int m_demoStep = 0;
    int m_demoFree = 14;
    int m_demoGateOpen = 0;
    int m_demoPlatesShown = 0;
};

Q_DECLARE_METATYPE(IpcSnapshot)

#endif /* IPC_READER_H */

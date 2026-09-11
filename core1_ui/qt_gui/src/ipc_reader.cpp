#include "ipc_reader.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QThread>

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================ shm (real mode) ============================ */

bool IpcReader::attachShm()
{
    if (m_shm)
        return true;
    int fd = ::shm_open("/park_shm", O_RDONLY, 0);
    if (fd < 0)
        return false;                       /* core0 not up yet: keep polling */
    struct stat st;
    if (::fstat(fd, &st) != 0 ||
        size_t(st.st_size) < sizeof(park_shm_t)) {
        ::close(fd);
        return false;
    }
    void *p = ::mmap(nullptr, sizeof(park_shm_t), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED)
        return false;
    m_shm = reinterpret_cast<park_shm_t *>(p);

    const park_shm_t *s = m_shm;
    if (s->magic != PARK_SHM_MAGIC || s->version != PARK_SHM_VERSION) {
        /* version mismatch: refuse stale layout (PhaseMd/07 P6-01 rule) */
        ::munmap(p, sizeof(park_shm_t));
        m_shm = nullptr;
        return false;
    }
    return true;
}

/* seq double-read consistency per P6-01 */
bool IpcReader::readShm(IpcSnapshot *out)
{
    park_shm_t tmp;
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s1 = m_shm->seq;
        memcpy(&tmp, m_shm, sizeof(tmp));
        const uint32_t s2 = m_shm->seq;
        if (s1 == s2 && s1 != 0) {
            out->shmOnline = true;
            out->demo = false;
            out->freeSlots = int(tmp.free_slots);
            out->usedSlots = int(tmp.used_slots);
            out->gateOpen = tmp.gate_state != 0;
            out->recogPending = tmp.recog_pending != 0;
            out->cloudPending = tmp.cloud_pending != 0;
            out->linkFlags = tmp.link_flags;
            out->plate = QString::fromUtf8(tmp.plate);
            out->confidence = tmp.confidence;
            out->source = int(tmp.result_source);
            return true;
        }
        QThread::msleep(1);
    }
    return false;
}

/* ============================ demo simulator ============================ */
/* ASCII-only demo plates except one CJK-prefixed case to verify fonts. */

static const char *kDemoPlates[] = {
    "\xE8\x8B\x8F""A12345",   /* SuA12345 */
    "\xE8\x8B\x8F""B67890",   /* SuB67890 */
    "A88888",
    "B12345"
};

void IpcReader::demoTick()
{
    ++m_demoStep;
    IpcSnapshot s;
    s.demo = true;
    s.shmOnline = false;

    /* slots: slow drift, occasionally dips below the orange threshold */
    if (m_demoStep % 7 == 0) {
        if (m_demoFree <= 3 || (m_demoFree < 18 && QRandomGenerator::global()->bounded(3) == 0))
            ++m_demoFree;
        else if (m_demoFree > 2 && QRandomGenerator::global()->bounded(3) == 0)
            --m_demoFree;
        emit eventMessage(QString("demo: slots -> free=%1").arg(m_demoFree));
    }

    /* gate toggles every ~8 ticks (8 s) with an event line */
    if (m_demoStep % 8 == 0) {
        m_demoGateOpen = !m_demoGateOpen;
        emit eventMessage(QString("demo: gate %1")
                          .arg(m_demoGateOpen ? "OPEN" : "CLOSE"));
    }

    s.freeSlots = m_demoFree;
    s.usedSlots = 20 - m_demoFree;
    s.gateOpen = m_demoGateOpen != 0;
    s.linkFlags = 0x07;                     /* rpmsg + m4 + core1 online */

    /* a recognition result roughly every 6 ticks */
    if (m_demoStep % 6 == 0) {
        const int i = m_demoPlatesShown++ % 4;
        const QString plate = QString::fromUtf8(kDemoPlates[i]);
        const double conf = 0.86 + double(QRandomGenerator::global()->bounded(12)) / 100.0;
        const int source = (m_demoPlatesShown % 2) ? 1 : 0;   /* alternate edge/cloud */
        const bool deny = (i == 3);                            /* B12345 not whitelisted */
        s.plate = plate;
        s.confidence = conf;
        s.source = source;
        s.recogPending = false;
        emit platePopup(plate, conf, source, deny);
        emit eventMessage(QString("demo: %1 %2 conf=%3")
                          .arg(deny ? "DENY" : (source ? "CLOUD PASS" : "EDGE PASS"),
                               plate).arg(conf, 0, 'f', 2));
    } else {
        s.recogPending = (m_demoStep % 6 == 4);   /* brief "recognizing" badge */
        if (s.recogPending)
            emit eventMessage(QString("demo: recognition started"));
    }

    m_snap = s;
    emit snapshotChanged(m_snap);
}

/* ============================ lifecycle ============================ */

IpcReader::IpcReader(QObject *parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &IpcReader::onTick);
    connect(&m_demoTimer, &QTimer::timeout, this, &IpcReader::demoTick);
}

IpcReader::~IpcReader()
{
    stop();
}

void IpcReader::start(DemoMode demoMode, int eventfdFd)
{
    if (demoMode == DemoForce) {
        m_demo = true;
    } else if (demoMode == DemoOff) {
        m_demo = false;
    } else {                                  /* DemoAuto: demo until shm shows up */
        m_demo = !attachShm();
    }

    if (m_demo) {
        m_demoTimer.start(1000);              /* 1 s scene cadence */
        emit eventMessage("demo mode: no /park_shm writer, simulating data");
        return;
    }

    if (eventfdFd >= 0) {
        m_eventfd = eventfdFd;
        m_evtNotifier = new QSocketNotifier(m_eventfd, QSocketNotifier::Read, this);
        connect(m_evtNotifier, &QSocketNotifier::activated,
                this, &IpcReader::onEventfd);
        m_timer.start(1000);                  /* 1 s fallback poll per spec */
    } else {
        m_timer.start(200);                   /* no eventfd yet: 200 ms poll */
    }
    onTick();
}

void IpcReader::stop()
{
    m_timer.stop();
    m_demoTimer.stop();
    delete m_evtNotifier;
    m_evtNotifier = nullptr;
    if (m_shm) {
        ::munmap(m_shm, sizeof(park_shm_t));
        m_shm = nullptr;
    }
}

void IpcReader::onTick()
{
    if (m_shm || attachShm()) {
        IpcSnapshot s;
        if (readShm(&s)) {
            if (m_demo) {                     /* auto mode: real writer appeared */
                m_demo = false;
                m_demoTimer.stop();
            }
            if (s.freeSlots != m_snap.freeSlots ||
                s.usedSlots != m_snap.usedSlots ||
                s.gateOpen != m_snap.gateOpen ||
                s.recogPending != m_snap.recogPending ||
                s.cloudPending != m_snap.cloudPending ||
                s.linkFlags != m_snap.linkFlags ||
                s.plate != m_snap.plate ||
                s.confidence != m_snap.confidence ||
                s.source != m_snap.source ||
                !m_snap.shmOnline) {
                m_snap = s;
                emit snapshotChanged(m_snap);
            }
        }
    } else if (m_demo) {
        demoTick();
    }
}

void IpcReader::onEventfd()
{
    uint64_t n = 0;
    while (::read(m_eventfd, &n, sizeof(n)) == ssize_t(sizeof(n))) {
        /* consume the accumulated counter; each wakeup triggers a refresh */
        if (n == 0)
            break;
    }
    onTick();
}

#include "ipc_writer.h"

#include <QByteArray>

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================ lifecycle ============================ */

IpcWriter::IpcWriter(QObject *parent)
    : QObject(parent)
{
    connect(&m_hbTimer, &QTimer::timeout, this, &IpcWriter::onHeartbeat);
    connect(&m_evtTimer, &QTimer::timeout, this, &IpcWriter::onEvtPoll);
}

IpcWriter::~IpcWriter()
{
    stop();
}

void IpcWriter::start()
{
    attachShm();
    m_hbTimer.start(1000);              /* spec 5.2.1.1 */
    m_evtTimer.start(200);              /* spec 5.7.1.3 (<=500ms budget) */
}

void IpcWriter::stop()
{
    m_hbTimer.stop();
    m_evtTimer.stop();
    if (m_shm) {
        ::munmap(m_shm, sizeof(park_shm_t));
        m_shm = nullptr;
    }
}

/* ============================ attach ============================ */

bool IpcWriter::attachShm()
{
    if (m_shm)
        return true;

    /* MUST be O_RDWR + PROT_WRITE: heartbeats/results are written by Core1 */
    int fd = ::shm_open("/park_shm", O_RDWR, 0);
    if (fd < 0) {
        /* core0 not up yet (ENOENT): keep retrying, never crash (spec 5.1.3.2).
         * Reported once so the journal stays readable. */
        if (!m_noShmLogged) {
            m_noShmLogged = true;
            emit eventMessage(QStringLiteral(
                "ipc-writer: /park_shm not ready yet (core0 not up?), retrying"));
        }
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || size_t(st.st_size) < sizeof(park_shm_t)) {
        ::close(fd);
        return false;
    }

    void *p = ::mmap(nullptr, sizeof(park_shm_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED)
        return false;

    park_shm_t *s = reinterpret_cast<park_shm_t *>(p);
    if (s->magic != PARK_SHM_MAGIC || s->version != PARK_SHM_VERSION) {
        /* version self-check (spec 5.1.1.2 / 5.1.3.1): refuse, ERROR, keep the
         * read-only display alive. Reported once per distinct version so the
         * 1s+200ms retry loop cannot flood the journal. */
        if (m_badVerLogged != s->version) {
            m_badVerLogged = s->version;
            emit eventMessage(QStringLiteral(
                "ipc-writer: ERROR /park_shm mismatch "
                "(magic=0x%1 version=%2, need 0x%3/%4): write-end refused")
                    .arg(s->magic, 0, 16).arg(s->version)
                    .arg(PARK_SHM_MAGIC, 0, 16).arg(PARK_SHM_VERSION));
        }
        ::munmap(p, sizeof(park_shm_t));
        return false;
    }

    m_shm = s;
    m_lastSeqC0 = s->evt_seq_c0;
    m_noShmLogged = false;
    m_badVerLogged = 0;
    emit eventMessage(QStringLiteral("ipc-writer: /park_shm attached (v%1)")
                          .arg(s->version));
    return true;
}

/* ============================ heartbeat ============================ */

void IpcWriter::onHeartbeat()
{
    if (!m_shm)
        attachShm();                    /* late attach keeps retrying */
    if (m_shm)
        __sync_add_and_fetch(&m_shm->hb_core1, 1);
}

/* ============================ recognition result ============================ */

void IpcWriter::onRecogResult(const QString &plate, double confidence,
                              int source)
{
    Q_UNUSED(source);                   /* K210 edge source is always 0 here */
    if (!m_shm && !attachShm()) {
        emit eventMessage(QStringLiteral(
            "ipc-writer: /park_shm not attached, result dropped"));
        return;
    }

    /* read live (no cache): Core0 is the config authority, a mid-run change
     * takes effect on the next result (spec 5.5.1.4) */
    float thr = m_shm->conf_threshold;
    if (!(thr > 0.0f && thr <= 1.0f)) {
        /* invalid threshold from Core0 (spec 5.5.3.1): warn once, keep the
         * business running on the documented 0.60 fallback */
        if (!m_badThrLogged) {
            m_badThrLogged = true;
            emit eventMessage(QStringLiteral(
                "ipc-writer: WARN bad conf_threshold %1, using 0.60")
                    .arg(double(thr), 0, 'f', 2));
        }
        thr = m_fallbackThreshold;
    } else {
        m_badThrLogged = false;
    }

    if (double(confidence) >= double(thr)) {
        publishResult(plate, confidence, 0);   /* high confidence: edge, src=0 */
        /* cloud_pending stays 0 */
    } else {
        m_shm->cloud_pending = 1;              /* low confidence -> cloud */
        emit cloudPendingChanged(true);
        emit eventMessage(QStringLiteral(
            "ipc-writer: low confidence %1 < %2 -> cloud pending")
                              .arg(confidence, 0, 'f', 2)
                              .arg(thr, 0, 'f', 2));
        /* step 7: hand the decision to the application (CloudClient) */
        emit cloudFallbackRequested(QStringLiteral("edge conf %1 < %2")
                                        .arg(confidence, 0, 'f', 2)
                                        .arg(thr, 0, 'f', 2));
    }
}

/* End of a cloud attempt with no usable plate: cloud_pending must go back to 0
 * so Core0 stops extending the recognition timeout (spec P7-04 / 5.4.1.3). */
void IpcWriter::clearCloudPending()
{
    if (!m_shm)
        return;
    if (m_shm->cloud_pending == 0)
        return;
    m_shm->cloud_pending = 0;
    emit cloudPendingChanged(false);
    emit eventMessage(QStringLiteral("ipc-writer: cloud_pending cleared"));
}

void IpcWriter::onRecogFailed(const QString &reason)
{
    if (!m_shm && !attachShm()) {
        emit eventMessage(QStringLiteral(
            "ipc-writer: /park_shm not attached, failure dropped"));
        return;
    }
    m_shm->cloud_pending = 1;                  /* do NOT set result_valid */
    emit cloudPendingChanged(true);
    emit eventMessage(QStringLiteral(
                          "ipc-writer: recog failed -> cloud pending: %1")
                          .arg(reason));
    emit cloudFallbackRequested(QStringLiteral("edge failed: %1").arg(reason));
}

void IpcWriter::onCloudResult(const QString &plate, double confidence)
{
    if (!m_shm && !attachShm())
        return;

    /* cloud fallback write-back (reserved for step 7): source=1 */
    QByteArray bytes = plate.toUtf8();
    if (bytes.size() > 15)
        bytes.truncate(15);
    memset(m_shm->plate, 0, sizeof(m_shm->plate));
    memcpy(m_shm->plate, bytes.constData(), size_t(bytes.size()));
    m_shm->confidence = float(confidence);
    m_shm->result_source = 1;                  /* cloud */
    __sync_synchronize();                      /* release: fields before valid */
    m_shm->result_valid = 1;
    m_shm->cloud_pending = 0;
    publishEvtC1(PARK_EVT_RESULT);
    /* spec 4.3.2: key writes (result write-back, cloud_pending clear) are
     * logged for audit */
    emit eventMessage(QStringLiteral(
        "ipc-writer: cloud result '%1' conf=%2 -> result_source=1, "
        "cloud_pending cleared").arg(plate).arg(confidence, 0, 'f', 2));
    emit cloudPendingChanged(false);
}

/* ============================ results / events (write order) ============================ */

void IpcWriter::publishResult(const QString &plate, double confidence,
                              int source)
{
    if (!m_shm)
        return;

    /* spec 6.1.4: Core0 clears result_valid once it has consumed the result.
     * A still-set flag means the previous result was not read yet; the
     * overwrite is harmless (Core0 charges the passage once) but audited. */
    if (m_shm->result_valid)
        emit eventMessage(QStringLiteral(
            "ipc-writer: WARN previous result not consumed yet, overwriting"));

    /* 1. plate[16]: UTF-8, truncated to 15 bytes + NUL (spec 6.1.1) */
    QByteArray bytes = plate.toUtf8();
    if (bytes.size() > 15) {
        bytes.truncate(15);
        emit eventMessage(QStringLiteral("ipc-writer: plate truncated to 15B"));
    }
    memset(m_shm->plate, 0, sizeof(m_shm->plate));
    memcpy(m_shm->plate, bytes.constData(), size_t(bytes.size()));

    /* 2. confidence, 3. result_source */
    m_shm->confidence = float(confidence);
    m_shm->result_source = uint8_t(source);

    /* 4. release barrier: everything above is visible before valid=1, so the
     *    Core0 reader (who reads fields then valid last) never sees a torn
     *    result. Never bump seq here (seq is Core0's whole-struct guard). */
    __sync_synchronize();

    /* 5. publish point */
    m_shm->result_valid = 1;

    /* 6-8. result-ready event */
    publishEvtC1(PARK_EVT_RESULT);
}

void IpcWriter::publishEvtC1(uint8_t code)
{
    if (!m_shm)
        return;
    m_shm->evt_bits_c1 |= uint8_t(PARK_EVB(code));
    __sync_synchronize();              /* bits visible before seq bump */
    __sync_add_and_fetch(&m_shm->evt_seq_c1, 1);
}

/* ============================ remote control ============================ */

void IpcWriter::requestGateOpen()
{
    if (!m_shm && !attachShm()) {
        emit eventMessage(QStringLiteral(
            "ipc-writer: gate open request dropped (no shm)"));
        return;
    }
    m_shm->req_gate_open = 1;           /* pulse; Core0 clears after exec */
    emit eventMessage(QStringLiteral("ipc-writer: gate open requested"));
}

void IpcWriter::requestGateClose()
{
    if (!m_shm && !attachShm()) {
        emit eventMessage(QStringLiteral(
            "ipc-writer: gate close request dropped (no shm)"));
        return;
    }
    m_shm->req_gate_close = 1;
    emit eventMessage(QStringLiteral("ipc-writer: gate close requested"));
}

/* ============================ c0 event consumption ============================ */

void IpcWriter::onEvtPoll()
{
    if (!m_shm) {
        attachShm();                    /* late-attach retry each tick */
        return;
    }

    const uint32_t seqC0 = m_shm->evt_seq_c0;
    if (seqC0 == m_lastSeqC0)
        return;
    const uint8_t bits = m_shm->evt_bits_c0;
    m_lastSeqC0 = seqC0;
    consumeEvtC0(bits);
}

void IpcWriter::consumeEvtC0(uint8_t bits)
{
    if (!m_shm)
        return;

    uint8_t handled = 0;
    if (bits & uint8_t(PARK_EVB_TRIGGER)) {
        handled |= uint8_t(PARK_EVB_TRIGGER);
        /* recognition is K210 continuous periodic; badge is driven by the
         * Core0-written recog_pending field, so this is a light consume + log */
        emit eventMessage(QStringLiteral("evt: recognition trigger"));
    }
    if (bits & uint8_t(PARK_EVB_STATE)) {
        handled |= uint8_t(PARK_EVB_STATE);
        emit snapshotRefreshRequested();
    }
    if (bits & uint8_t(PARK_EVB_RESYNC)) {
        handled |= uint8_t(PARK_EVB_RESYNC);
        emit snapshotRefreshRequested();
    }

    if (handled) {
        /* consumer-clear: only clear handled bits, never set them and never
         * bump evt_seq_c0 (that is Core0's publish responsibility). This is
         * the symmetric counterpart of Core0's evt_take_c1 consumer-clear. */
        __sync_fetch_and_and(&m_shm->evt_bits_c0, uint8_t(~handled));
    }
}
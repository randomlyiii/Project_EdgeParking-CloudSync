#ifndef IPC_WRITER_H
#define IPC_WRITER_H
/* IpcWriter - Core1 shared-memory writer (runs inside the park_ui process).
 *
 * Complements the read-only IpcReader: while IpcReader takes full-structure
 * snapshots for display, IpcWriter owns the Core1-owned fields of /park_shm
 * (docs/protocols.md section 4, park_shm v3):
 *   hb_core1, cloud_pending, result_valid, result_source, confidence,
 *   plate[16], req_gate_open, req_gate_close, evt_seq_c1, evt_bits_c1
 * and consumes Core0-originated events (evt_seq_c0 / evt_bits_c0).
 *
 * It MUST NEVER write the Core0-owned fields (magic/version/seq/slots/
 * gate_state/link_flags/recog_pending/fault_bits/conf_threshold/evt_*_c0
 * except the consumer-clear of evt_bits_c0) and MUST NEVER bump seq.
 *
 * Threading: everything runs on the main thread (heartbeat and event-poll
 * are QTimers; recognition results arrive via queued signals from K210Link's
 * worker thread). No internal locking is needed.
 */
#include <QObject>
#include <QTimer>

#include "park_shm.h"

class IpcWriter : public QObject
{
    Q_OBJECT
public:
    explicit IpcWriter(QObject *parent = nullptr);
    ~IpcWriter() override;

    void start();                       /* attach /park_shm, then arm timers */
    void stop();                        /* stop timers + munmap */
    bool isAttached() const { return m_shm != nullptr; }

public slots:
    /* K210 recognition result (source is always 0 = edge from K210) */
    void onRecogResult(const QString &plate, double confidence, int source);
    void onRecogFailed(const QString &reason);
    /* remote control pulses (Core0 clears after executing) */
    void requestGateOpen();
    void requestGateClose();
    /* cloud fallback write-back (reserved for step 7; source=1) */
    void onCloudResult(const QString &plate, double confidence);

signals:
    void eventMessage(const QString &line);
    void cloudPendingChanged(bool pending);
    void snapshotRefreshRequested();    /* STATE/RESYNC c0 event -> UI refresh */

private slots:
    void onHeartbeat();                 /* 1000 ms: hb_core1 atomic increment */
    void onEvtPoll();                   /* 200 ms: poll c0 event channel */

private:
    bool attachShm();
    void publishResult(const QString &plate, double confidence, int source);
    void publishEvtC1(uint8_t code);
    void consumeEvtC0(uint8_t bits);

    park_shm_t *m_shm = nullptr;        /* MAP_SHARED, null until attach ok */
    QTimer m_hbTimer;
    QTimer m_evtTimer;
    uint32_t m_lastSeqC0 = 0;
    float m_fallbackThreshold = 0.60f;
    /* log throttles: attachShm() is retried by two timers, so the refusal
     * paths must not spam the journal (spec 5.1.3: report, do not flood) */
    uint32_t m_badVerLogged = 0;        /* last version already reported */
    bool m_noShmLogged = false;         /* "/park_shm not ready" reported */
    bool m_badThrLogged = false;        /* bad conf_threshold reported */
};

#endif /* IPC_WRITER_H */
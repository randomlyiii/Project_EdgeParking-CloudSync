#ifndef CLOUD_CLIENT_H
#define CLOUD_CLIENT_H
/* CloudClient - DeepSeek-compatible vision fallback, step 7 (P7-01..P7-07).
 *
 * Lives inside park_ui (Core1): the spec forbids Core0 from making cloud
 * requests. Transport is Qt Network (QNetworkAccessManager) instead of
 * libcurl - the board's Qt 5.12.8 SDK already ships QtNetwork, so no extra
 * cross-compile dependency is needed, and the async API keeps the UI thread
 * free while the request is in flight.
 *
 * Guarantees required by PhaseMd/08:
 *   - single-shot: one request per recognition event, never a polling loop;
 *   - timeoutMs (default 5000) hard cap via a QTimer + abort();
 *   - retry <= 1, and only for transport problems (never for 4xx);
 *   - tolerant parsing: markdown fences / surrounding prose / truncated JSON;
 *   - API key is never logged (log cloud_settings_mask_key() instead).
 *
 * The client does NOT touch /park_shm: the owner of cloud_pending /
 * result_valid is IpcWriter, which the application wires to these signals.
 */
#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QProcess>     /* QProcess::ExitStatus is used in a slot signature */
#include <QString>
#include <QTimer>

#include "cloud_settings.h"

class QNetworkAccessManager;
class QNetworkReply;

struct CloudStats {
    int     ok = 0;          /* usable answers (plate accepted) */
    int     fail = 0;        /* transport / protocol / below-threshold failures */
    int     unreadable = 0;  /* HTTP OK but the model read no plate */
    qint64  lastMs = 0;
    qint64  totalMs = 0;
    QString lastError;       /* ASCII, key-free */
    QString lastPlate;
    double  lastConf = 0.0;
    bool    busy = false;
};

class CloudClient : public QObject
{
    Q_OBJECT
public:
    explicit CloudClient(QObject *parent = nullptr);
    ~CloudClient() override;

    void setSettings(const CloudSettings &s)
    {
        m_s = s;
        m_pythonOnly = false;      /* a config change re-tries the Qt path */
    }
    const CloudSettings &settings() const { return m_s; }
    CloudStats stats() const { return m_stats; }
    bool busy() const { return m_stats.busy; }

    /* Short human-readable status for the CLOUD chip. */
    QString statusText() const;

    /* Absolute path of the CA bundle that would be used for this settings set
     * ("" = none found). The settings page shows it, tests use it. */
    static QString caBundlePathFor(const CloudSettings &s);

    /* Human-readable proxy mode for logs and the settings page. */
    static QString proxyModeText(const CloudSettings &s);
    /* "auto (qt -> python)" / "qt" / "python" for the settings page. */
    static QString transportText(const CloudSettings &s);

    /* Outage drill (P7-11 demo): every request fails locally after ~1.2s with
     * no packet leaving the board, proving the local loop keeps working. */
    void setOutageSimulation(bool on) { m_outage = on; }
    bool outageSimulation() const { return m_outage; }

public slots:
    /* Ask the cloud about one frame. reason is free text for the log line.
     * writeback=true means the caller will feed the answer back to Core0. */
    void recognize(const QImage &frame, bool writeback, const QString &reason);
    /* Minimal round trip used by the "test connection" button. */
    void testConnection();
    void cancel();

signals:
    void started(const QString &what);
    void finished(const QString &plate, double confidence, int httpStatus,
                  qint64 ms, bool writeback);
    /* HTTP OK but no plate readable (valid answer, keeps the caller in the
     * normal downgrade path) */
    void unreadable(const QString &detail, qint64 ms);
    void failed(const QString &reason, const QString &detail);
    void eventMessage(const QString &line);
    void statsChanged();

private slots:
    void onReplyFinished();
    void onTimeout();
    void onPythonFinished(int exitCode, QProcess::ExitStatus status);

private:
    void send(const QByteArray &body, bool isTest, bool writeback,
              const QString &reason);
    void applyProxy();
    /* which transport serves the next request (see transport= in cloud.conf) */
    bool pythonTransportActive() const;
    /* python3 fallback transport (see the note in the .cpp) */
    void sendViaPython(bool isTest, bool writeback, const QString &reason);
    /* one place that turns a successful HTTP body into signals */
    void processBody(const QByteArray &body, int status, qint64 ms);
    void sendFake(bool isTest, bool writeback, const QString &reason);
    void finishFail(const QString &reason, const QString &detail);
    void finishUnreadable(const QString &detail, qint64 ms);
    void updateStats(bool ok, qint64 ms, const QString &err);

    static QByteArray encodeJpeg(const QImage &frame, int quality);
    static bool extractContent(const QByteArray &body, QString *out,
                               QString *err);
    static bool parsePlateJson(const QString &content, QString *plate,
                               double *conf, QString *err);
    static QString truncateUtf8(const QString &s, int maxBytes);
    static QString httpHint(int status);
    static bool retryable(QNetworkReply *reply);
    QByteArray buildBody(const QImage *frame) const;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QProcess *m_py = nullptr;      /* live python transport child, if any */
    QTimer m_timer;
    CloudSettings m_s;
    CloudStats m_stats;
    QElapsedTimer m_clock;
    QByteArray m_lastJpeg;         /* encoded frame for the python transport */
    bool m_pythonOnly = false;     /* auto transport already fell back */

    /* retry state */
    QByteArray m_lastBody;
    bool m_lastTest = false;
    bool m_lastWriteback = false;
    QString m_lastReason;
    int m_attempt = 0;
    bool m_timedOut = false;
    bool m_retryPending = false;   /* 300 ms retry window: refuse new requests */
    int m_gen = 0;                 /* cancel() invalidates pending timers */
    bool m_caLogged = false;       /* TLS/CA state is logged once */
    bool m_forceTls12 = false;     /* pin TLS 1.2 (Qt/OpenSSL 1.1.1 stall) */
    bool m_tls12Retry = false;     /* the 1.2 retry already happened once */
    bool m_outage = false;      /* outage drill: fail without sending */
};

#endif /* CLOUD_CLIENT_H */

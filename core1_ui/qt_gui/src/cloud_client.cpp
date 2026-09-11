#include "cloud_client.h"

#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>
#include <QVariant>

/* Demo plate used by the fake mode: it is the whitelisted example entry of
 * core0.conf.example, so the whole chain can be demonstrated without network. */
static const char *kFakePlate = "TEST001";

CloudClient::CloudClient(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_s = cloud_settings_defaults();
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &CloudClient::onTimeout);
}

CloudClient::~CloudClient()
{
    cancel();
}

QString CloudClient::statusText() const
{
    if (m_stats.busy)
        return QStringLiteral("busy");
    if (m_stats.ok + m_stats.fail + m_stats.unreadable == 0)
        return QStringLiteral("idle");
    if (!m_stats.lastError.isEmpty())
        return QStringLiteral("fail");
    if (!m_s.apiKey.isEmpty())
        return QStringLiteral("ok");
    return QStringLiteral("no key");
}

/* ------------------------------------------------------------------ */
/* request construction                                                */

/* First readable CA bundle. The board rootfs ships none (probed 2026-09-11),
 * so the operator can drop one at /etc/park/ca.pem or name it with ca_file=
 * in cloud.conf. */
static const char *const kBoardCaPem = "/etc/park/ca.pem";
/* Where the python fallback keeps its helper and the (live) job file. /tmp is
 * tmpfs here, so the job never reaches the flash. */
static const char *const kPyScriptPath = "/tmp/park_cloud_req.py";
static const char *const kPyJobPath = "/tmp/park_cloud_job.json";

/* python3 lives in /bin on this board, and a systemd unit's PATH does not
 * necessarily contain /bin - resolve the interpreter explicitly. */
static QString pythonPath()
{
    static const char *const cands[] = {
        "/bin/python3", "/usr/bin/python3", "/usr/local/bin/python3",
        "/bin/python", "/usr/bin/python",
    };
    for (int i = 0; i < 5; ++i) {
        if (QFile::exists(QString::fromLatin1(cands[i])))
            return QString::fromLatin1(cands[i]);
    }
    return QStringLiteral("python3");
}

QString CloudClient::caBundlePathFor(const CloudSettings &s)
{
    QStringList cands;
    if (!s.caFile.trimmed().isEmpty())
        cands << s.caFile.trimmed();
    cands << QString::fromLatin1(kBoardCaPem)
          << QStringLiteral("/etc/ssl/certs/ca-certificates.crt")
          << QStringLiteral("/etc/ssl/certs/ca-bundle.crt")
          << QStringLiteral("/etc/pki/tls/certs/ca-bundle.crt")
          << QStringLiteral("/etc/ssl/cert.pem");
    for (int i = 0; i < cands.size(); ++i) {
        if (QFile::exists(cands.at(i)))
            return cands.at(i);
    }
    return QString();
}

/* Which transport should serve this request?
 *   transport=python -> always python3
 *   transport=qt     -> always Qt (never falls back)
 *   transport=auto   -> Qt, and after the first failure stay on python3    */
bool CloudClient::pythonTransportActive() const
{
    const QString t = m_s.transport.trimmed().toLower();
    if (t == QLatin1String("python") || t == QLatin1String("py"))
        return true;
    if (t == QLatin1String("qt") || t == QLatin1String("network"))
        return false;
    return m_pythonOnly;             /* auto */
}

QString CloudClient::transportText(const CloudSettings &s)
{
    const QString t = s.transport.trimmed().toLower();
    if (t.isEmpty() || t == QLatin1String("auto"))
        return QStringLiteral("auto (qt -> python)");
    return t;
}

/* Proxy policy. Default is NO proxy: this board's shell/system may carry a
 * stray https_proxy (campus network image), and Qt - unlike python - picks it
 * up and then silently connects to a dead proxy, which shows up as a request
 * that burns the full timeout while a direct connection works fine. */
QString CloudClient::proxyModeText(const CloudSettings &s)
{
    const QString p = s.proxy.trimmed();
    if (p.isEmpty())
        return QStringLiteral("none (direct)");
    if (p.compare(QLatin1String("env"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("environment");
    return p;
}

/* Readable name for the negotiated protocol. Deliberately avoids both
 * QSslConfiguration::protocolString() (not in Qt 5.12) and QSsl::TlsV1_3
 * (a later enum value): only values that certainly exist get a name, the rest
 * is printed numerically. 5 == TLS 1.3 on builds that have it. */
static QString tlsProtocolName(QSsl::SslProtocol p)
{
    switch (p) {
    case QSsl::SslV3:   return QStringLiteral("SSLv3");
    case QSsl::TlsV1_0: return QStringLiteral("TLS1.0");
    case QSsl::TlsV1_1: return QStringLiteral("TLS1.1");
    case QSsl::TlsV1_2: return QStringLiteral("TLS1.2");
    default:            break;
    }
    if (int(p) == 5)
        return QStringLiteral("TLS1.3");
    return QStringLiteral("protocol%1").arg(int(p));
}

void CloudClient::applyProxy()
{
    const QString p = m_s.proxy.trimmed();
    if (p.isEmpty()) {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        m_nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        return;
    }
    if (p.compare(QLatin1String("env"), Qt::CaseInsensitive) == 0) {
        m_nam->setProxy(QNetworkProxy(QNetworkProxy::DefaultProxy));
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        qWarning("cloud: proxy = environment (http_proxy/https_proxy)");
        return;
    }
    const QUrl u(p);
    QNetworkProxy proxy;
    proxy.setType(QNetworkProxy::HttpProxy);
    proxy.setHostName(u.host().isEmpty() ? p : u.host());
    proxy.setPort(quint16(u.port(8080)));
    if (!u.userName().isEmpty())
        proxy.setUser(u.userName());
    if (!u.password().isEmpty())
        proxy.setPassword(u.password());
    m_nam->setProxy(proxy);
    qWarning("cloud: proxy = %s:%d", qPrintable(proxy.hostName()),
             int(proxy.port()));
}

QByteArray CloudClient::encodeJpeg(const QImage &frame, int quality)
{
    QByteArray raw;
    QBuffer buf(&raw);
    buf.open(QIODevice::WriteOnly);
    if (!frame.save(&buf, "JPEG", quality))
        return QByteArray();
    buf.close();
    return raw;
}

QByteArray CloudClient::buildBody(const QImage *frame) const
{
    QJsonArray content;

    QJsonObject text;
    text.insert(QStringLiteral("type"), QStringLiteral("text"));
    text.insert(QStringLiteral("text"), m_s.prompt);
    content.append(text);

    if (frame != nullptr && !frame->isNull()) {
        const QByteArray jpeg = encodeJpeg(*frame, 70);
        if (!jpeg.isEmpty()) {
            QJsonObject url;
            url.insert(QStringLiteral("url"),
                       QStringLiteral("data:image/jpeg;base64,") +
                           QString::fromLatin1(jpeg.toBase64()));
            QJsonObject img;
            img.insert(QStringLiteral("type"), QStringLiteral("image_url"));
            img.insert(QStringLiteral("image_url"), url);
            content.append(img);
        } else {
            /* No qjpeg plugin in the rootfs, or a null frame: sending a
             * text-only request would let the model invent a plate. */
            qWarning("cloud: JPEG encode failed (%dx%d) - request has no image",
                     frame->width(), frame->height());
        }
    }

    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("user"));
    msg.insert(QStringLiteral("content"), content);

    QJsonArray messages;
    messages.append(msg);

    QJsonObject root;
    root.insert(QStringLiteral("model"), m_s.model);
    root.insert(QStringLiteral("messages"), messages);
    root.insert(QStringLiteral("max_tokens"), 64);
    root.insert(QStringLiteral("temperature"), 0);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */

void CloudClient::recognize(const QImage &frame, bool writeback,
                            const QString &reason)
{
    if (m_stats.busy || m_reply != nullptr || m_retryPending) {
        emit eventMessage(QStringLiteral("cloud: busy, request ignored"));
        return;
    }
    m_tls12Retry = false;          /* a fresh call may try TLS 1.3 again */
    m_forceTls12 = false;
    /* remember the frame: the python transport needs the raw JPEG, not JSON */
    m_lastJpeg = encodeJpeg(frame, 70);
    if (m_outage) {
        /* outage drill: behave like an unreachable cloud, nothing is sent */
        m_stats.busy = true;
        m_clock.start();
        emit started(reason);
        emit statsChanged();
        const int gen = m_gen;
        QTimer::singleShot(1200, this, [this, gen]() {
            if (gen != m_gen)                       /* cancelled */
                return;
            m_stats.busy = false;
            finishFail(QStringLiteral("simulated outage"),
                       QStringLiteral("drill: no request sent (P7-11)"));
        });
        return;
    }
    if (m_s.fakeResult) {
        sendFake(false, writeback, reason);
        return;
    }
    if (!cloud_settings_has_key(m_s)) {
        finishFail(QStringLiteral("no api key"),
                   QStringLiteral("set it in settings -> cloud"));
        return;
    }
    if (pythonTransportActive()) {
        sendViaPython(false, writeback, reason);
        return;
    }
    send(buildBody(&frame), false, writeback, reason);
}

void CloudClient::testConnection()
{
    if (m_stats.busy || m_reply != nullptr || m_retryPending) {
        emit eventMessage(QStringLiteral("cloud: busy, test ignored"));
        return;
    }
    m_tls12Retry = false;
    m_forceTls12 = false;
    if (m_outage) {
        emit eventMessage(QStringLiteral("cloud: outage drill active"));
        finishFail(QStringLiteral("simulated outage"),
                   QStringLiteral("drill: no request sent (P7-11)"));
        return;
    }
    if (m_s.fakeResult) {
        sendFake(true, false, QStringLiteral("test"));
        return;
    }
    if (!cloud_settings_has_key(m_s)) {
        finishFail(QStringLiteral("no api key"),
                   QStringLiteral("set it in settings -> cloud"));
        return;
    }
    if (pythonTransportActive()) {
        sendViaPython(true, false, QStringLiteral("test"));
        return;
    }
    /* text-only round trip: cheapest possible proof that endpoint + key work */
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("user"));
    msg.insert(QStringLiteral("content"), QStringLiteral("Reply with: ok"));
    QJsonArray messages;
    messages.append(msg);
    QJsonObject root;
    root.insert(QStringLiteral("model"), m_s.model);
    root.insert(QStringLiteral("messages"), messages);
    root.insert(QStringLiteral("max_tokens"), 4);
    root.insert(QStringLiteral("temperature"), 0);
    send(QJsonDocument(root).toJson(QJsonDocument::Compact), true, false,
         QStringLiteral("test"));
}

void CloudClient::sendFake(bool isTest, bool writeback, const QString &reason)
{
    m_stats.busy = true;
    m_clock.start();
    emit started(isTest ? QStringLiteral("test") : reason);
    emit statsChanged();
    /* canned answer after a short delay so the UI flow looks real */
    const int gen = m_gen;
    QTimer::singleShot(800, this, [this, isTest, writeback, gen]() {
        if (gen != m_gen)                       /* cancelled in the meantime */
            return;
        const qint64 ms = m_clock.elapsed();
        m_stats.busy = false;
        if (isTest) {
            m_stats.ok++;
            m_stats.lastMs = ms;
            m_stats.totalMs += ms;
            emit finished(QString(), 0.0, 0, ms, false);
            emit eventMessage(QStringLiteral(
                "cloud: FAKE mode - test answered locally (no network used)"));
            emit statsChanged();
            return;
        }
        m_stats.ok++;
        m_stats.lastMs = ms;
        m_stats.totalMs += ms;
        m_stats.lastPlate = QString::fromLatin1(kFakePlate);
        m_stats.lastConf = 0.98;
        emit finished(m_stats.lastPlate, 0.98, 0, ms, writeback);
        emit eventMessage(QStringLiteral(
            "cloud: FAKE mode - canned result (no network used)"));
        emit statsChanged();
    });
}

void CloudClient::send(const QByteArray &body, bool isTest, bool writeback,
                       const QString &reason)
{
    m_lastBody = body;
    m_lastTest = isTest;
    m_lastWriteback = writeback;
    m_lastReason = reason;
    m_timedOut = false;

    applyProxy();               /* explicit, never inherited by accident */

    const QUrl url(m_s.apiBase);
    if (!url.isValid() || url.scheme().isEmpty()) {
        finishFail(QStringLiteral("bad api base"), m_s.apiBase);
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + m_s.apiKey.toUtf8());
    req.setRawHeader("User-Agent", "park_ui/step7");
    if (m_s.insecureTls) {
        QSslConfiguration cfg = req.sslConfiguration();
        cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        if (m_forceTls12)
            cfg.setProtocol(QSsl::TlsV1_2);
        req.setSslConfiguration(cfg);
        qWarning("cloud: TLS peer verification DISABLED (insecure_tls=1)");
    } else {
        /* This rootfs has NO /etc/ssl/certs/ca-certificates.crt (probed
         * 2026-09-11): without a bundle Qt cannot verify api.deepseek.com and
         * every HTTPS request fails with a handshake error. Use an explicit
         * ca_file, then the well-known locations, and say so loudly once. */
        const QString ca = caBundlePathFor(m_s);
        if (!ca.isEmpty()) {
            const QList<QSslCertificate> certs =
                QSslCertificate::fromPath(ca);
            if (!certs.isEmpty()) {
                QSslConfiguration cfg = req.sslConfiguration();
                cfg.setCaCertificates(certs);
                if (m_forceTls12) {
                    /* Qt 5.12 + OpenSSL 1.1.1 can stall while negotiating
                     * TLS 1.3 with some servers; the retry pins 1.2. */
                    cfg.setProtocol(QSsl::TlsV1_2);
                    qWarning("cloud: retrying with TLS 1.2 pinned");
                }
                req.setSslConfiguration(cfg);
                if (!m_caLogged) {
                    m_caLogged = true;
                    qWarning("cloud: using CA bundle %s (%d certificates)",
                             qPrintable(ca), int(certs.size()));
                }
            } else if (!m_caLogged) {
                m_caLogged = true;
                qWarning("cloud: CA file %s has no readable certificate",
                         qPrintable(ca));
            }
        } else if (!m_caLogged) {
            m_caLogged = true;
            qWarning("cloud: no CA bundle found (ca_file=, %s, "
                     "/etc/ssl/certs/ca-certificates.crt ...) - HTTPS will "
                     "fail; install one or enable insecure_tls",
                     kBoardCaPem);
        }
    }

    m_stats.busy = true;
    m_clock.start();
    emit started(isTest ? QStringLiteral("test") : reason);
    emit statsChanged();
    emit eventMessage(QStringLiteral("cloud: %1 request -> %2 (model=%3, %4B)")
                          .arg(m_attempt == 0 ? QStringLiteral("first")
                                              : QStringLiteral("retry"))
                          .arg(url.host())
                          .arg(m_s.model)
                          .arg(body.size()));

    m_reply = m_nam->post(req, body);
    connect(m_reply, &QNetworkReply::finished, this,
            &CloudClient::onReplyFinished);
    m_timer.start(m_s.timeoutMs);
}

void CloudClient::cancel()
{
    m_timer.stop();
    m_gen++;                    /* invalidates pending outage/fake/retry timers */
    m_retryPending = false;
    if (m_reply != nullptr) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    if (m_stats.busy) {
        m_stats.busy = false;
        emit statsChanged();
    }
}

void CloudClient::onTimeout()
{
    if (m_reply == nullptr)
        return;
    m_timedOut = true;
    qWarning("cloud: timeout after %d ms -> abort", m_s.timeoutMs);
    m_reply->abort();
}

bool CloudClient::retryable(QNetworkReply *reply)
{
    if (reply == nullptr)
        return false;
    switch (reply->error()) {
    case QNetworkReply::TimeoutError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::OperationCanceledError:
        return true;
    default:
        return false;
    }
}

QString CloudClient::httpHint(int status)
{
    switch (status) {
    case 400: return QStringLiteral("400 bad request (model or prompt?)");
    case 401: return QStringLiteral("401 unauthorized - check the api key");
    case 402: return QStringLiteral("402 payment required - account balance");
    case 403: return QStringLiteral("403 forbidden");
    case 404: return QStringLiteral("404 - wrong endpoint path");
    case 429: return QStringLiteral("429 rate limited - retry later");
    default:
        if (status >= 500)
            return QStringLiteral("%1 server error").arg(status);
        break;
    }
    return QStringLiteral("http status %1").arg(status);
}

void CloudClient::onReplyFinished()
{
    if (m_reply == nullptr)
        return;
    m_timer.stop();

    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const qint64 ms = m_clock.elapsed();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError err = reply->error();
    const QByteArray body = reply->readAll();
    const QString errStr = reply->errorString();
    reply->deleteLater();

    if (err != QNetworkReply::NoError) {
        QString reason;
        if (m_timedOut)
            reason = QStringLiteral("timeout");
        else if (err == QNetworkReply::SslHandshakeFailedError)
            reason = QStringLiteral("tls handshake failed");
        else if (status > 0)
            reason = httpHint(status);
        else
            reason = QStringLiteral("network error");

        QString detail = errStr;
        if (err == QNetworkReply::SslHandshakeFailedError)
            detail += QStringLiteral(
                " | check /etc/ssl/certs or enable insecure_tls (demo)");
        if (status > 0)
            detail += QStringLiteral(" | http %1").arg(status);

        /* transport=auto: Qt could not complete it (on this board Qt 5.12 +
         * OpenSSL 1.1.1 stalls on TLS 1.3 while python gets HTTP 200 in 1 s).
         * Hand the request to python3 and stay there for this process. */
        if (!m_pythonOnly &&
            m_s.transport.trimmed().toLower() != QLatin1String("qt")) {
            m_pythonOnly = true;
            m_attempt = 0;
            m_retryPending = false;
            m_stats.busy = false;
            qWarning("cloud: qt transport failed (%s) - switching to python3",
                     qPrintable(reason));
            emit eventMessage(QStringLiteral(
                "cloud: qt failed (%1), retrying via python transport")
                                  .arg(reason));
            sendViaPython(m_lastTest, m_lastWriteback, m_lastReason);
            return;
        }

        /* Qt 5.12 + OpenSSL 1.1.1 can stall on TLS 1.3 with some endpoints
         * (measured on this board: python negotiates TLS 1.3 in 200 ms while
         * the same request in Qt burns the whole 5 s timeout). When the very
         * first attempt hangs or fails the handshake, retry once with the
         * protocol pinned to TLS 1.2 instead of reporting a dead cloud. */
        if (!m_tls12Retry && !m_forceTls12 &&
            (m_timedOut || err == QNetworkReply::SslHandshakeFailedError)) {
            m_tls12Retry = true;
            m_forceTls12 = true;
            m_attempt = 0;
            m_retryPending = true;
            m_stats.busy = false;
            qWarning("cloud: %s with TLS 1.3 - retrying once pinned to 1.2",
                     qPrintable(reason));
            emit eventMessage(QStringLiteral(
                "cloud: %1, retrying with TLS 1.2").arg(reason));
            const int gen = m_gen;
            QTimer::singleShot(200, this, [this, gen]() {
                if (gen != m_gen)
                    return;
                m_retryPending = false;
                if (m_stats.busy || m_reply != nullptr)
                    return;
                send(m_lastBody, m_lastTest, m_lastWriteback, m_lastReason);
            });
            return;
        }

        /* One retry, transport errors only. Never after our own hard timeout:
         * that would double the worst case to ~2x timeoutMs and break the 5 s
         * cap required by P7-04. */
        if (!m_timedOut && retryable(reply) && m_attempt < m_s.retry) {
            m_attempt++;
            m_stats.busy = false;
            m_retryPending = true;          /* no new request in this window */
            emit eventMessage(QStringLiteral("cloud: %1, retrying once")
                                  .arg(reason));
            const int gen = m_gen;
            QTimer::singleShot(300, this, [this, gen]() {
                if (gen != m_gen)           /* cancelled in the meantime */
                    return;
                m_retryPending = false;
                if (m_stats.busy || m_reply != nullptr)
                    return;                 /* someone else got in first */
                send(m_lastBody, m_lastTest, m_lastWriteback, m_lastReason);
            });
            return;
        }
        m_attempt = 0;
        m_retryPending = false;
        m_stats.busy = false;
        updateStats(false, ms, reason);
        emit failed(reason, detail);
        return;
    }

    m_attempt = 0;
    m_retryPending = false;
    m_stats.busy = false;
    m_timedOut = false;

    /* log what TLS actually negotiated: invaluable when a board's Qt/OpenSSL
     * combination behaves differently from the same request in python */
    const QSslConfiguration sslcfg = reply->sslConfiguration();
    qWarning("cloud: %s ok in %lld ms, tls=%s cipher=%s",
             m_lastTest ? "test" : "request", static_cast<long long>(ms),
             qPrintable(tlsProtocolName(sslcfg.protocol())),
             qPrintable(sslcfg.sessionCipher().name()));

    processBody(body, status, ms);
}

/* Everything that happens with a *successful* HTTP body. Shared by the Qt
 * transport and the python fallback so the parsing rules cannot diverge. */
void CloudClient::processBody(const QByteArray &body, int status, qint64 ms)
{
    if (m_lastTest) {
        m_stats.ok++;
        m_stats.lastMs = ms;
        m_stats.totalMs += ms;
        m_stats.lastError.clear();
        emit finished(QString(), 0.0, status, ms, false);
        emit eventMessage(QStringLiteral(
            "cloud: test OK (http %1, %2 ms, model=%3)")
                              .arg(status)
                              .arg(ms)
                              .arg(m_s.model));
        emit statsChanged();
        return;
    }

    QString content;
    QString perr;
    if (!extractContent(body, &content, &perr)) {
        updateStats(false, ms, perr);
        emit failed(perr, QStringLiteral("body %1B").arg(body.size()));
        return;
    }

    QString plate;
    double conf = 0.0;
    if (!parsePlateJson(content, &plate, &conf, &perr)) {
        updateStats(false, ms, perr);
        emit failed(perr, content.left(120));
        return;
    }
    if (plate.isEmpty()) {
        m_stats.unreadable++;
        m_stats.lastMs = ms;
        m_stats.totalMs += ms;
        m_stats.lastConf = conf;
        emit statsChanged();
        finishUnreadable(QStringLiteral("cloud read no plate (conf %1)")
                             .arg(conf, 0, 'f', 2),
                         ms);
        return;
    }
    if (conf < m_s.acceptConf) {
        m_stats.unreadable++;
        m_stats.lastMs = ms;
        m_stats.totalMs += ms;
        m_stats.lastConf = conf;
        emit statsChanged();
        finishUnreadable(QStringLiteral("cloud conf %1 below accept %2")
                             .arg(conf, 0, 'f', 2)
                             .arg(m_s.acceptConf, 0, 'f', 2),
                         ms);
        return;
    }

    m_stats.ok++;
    m_stats.lastMs = ms;
    m_stats.totalMs += ms;
    m_stats.lastPlate = plate;
    m_stats.lastConf = conf;
    m_stats.lastError.clear();
    emit finished(plate, conf, status, ms, m_lastWriteback);
    emit eventMessage(QStringLiteral("cloud: '%1' conf=%2 in %3 ms")
                          .arg(plate)
                          .arg(conf, 0, 'f', 2)
                          .arg(ms));
    emit statsChanged();
}

void CloudClient::finishUnreadable(const QString &detail, qint64 ms)
{
    emit unreadable(detail, ms > 0 ? ms : m_clock.elapsed());
    emit eventMessage(QStringLiteral("cloud: %1").arg(detail));
}

/* ------------------------------------------------------------------ */
/* python3 fallback transport                                          */
/*
 * Measured on the MP157 (2026-09-11): python3 gets HTTP 200 from the very same
 * endpoint in ~1 s while the identical request through Qt 5.12.8 + OpenSSL
 * 1.1.1 burns the whole timeout (network, clock, CA and key all verified good
 * from the shell). So when Qt cannot complete a request, hand it to python:
 * urllib + ssl is in the rootfs, needs no rebuild and is proven to work.
 *
 * The script is embedded (self-contained binary, nothing to deploy). The job -
 * which contains the API key - goes to a 0600 temp file that is removed as soon
 * as the child exits; only the mask is ever logged.
 */
static const char *const kPyTransport = R"PY(
import base64, json, os, ssl, sys, urllib.error, urllib.request

job = json.load(open(sys.argv[1]))
out = {"ok": False, "status": 0, "body": "", "error": ""}
try:
    content = [{"type": "text", "text": job.get("prompt", "")}]
    img = job.get("image") or ""
    if img and os.path.exists(img):
        fh = open(img, "rb")
        b64 = base64.b64encode(fh.read()).decode("ascii")
        fh.close()
        content.append({"type": "image_url",
                        "image_url": {"url": "data:image/jpeg;base64," + b64}})
    body = json.dumps({"model": job["model"],
                       "messages": [{"role": "user", "content": content}],
                       "max_tokens": int(job.get("max_tokens", 64)),
                       "temperature": 0}).encode("utf-8")
    req = urllib.request.Request(
        job["api_base"], data=body,
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + job["api_key"],
                 "Accept": "application/json",
                 "User-Agent": "park_ui/step7-py"})
    ca = job.get("ca_file") or ""
    if job.get("insecure"):
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    elif ca and os.path.exists(ca):
        ctx = ssl.create_default_context(cafile=ca)
    else:
        ctx = ssl.create_default_context()
    try:
        r = urllib.request.urlopen(req, timeout=float(job.get("timeout_s", 6)),
                                   context=ctx)
        out["ok"] = True
        out["status"] = r.status
        out["body"] = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        out["status"] = e.code
        out["error"] = "http " + str(e.code)
        out["body"] = e.read().decode("utf-8", "replace")
except Exception as e:
    out["error"] = "%s: %s" % (type(e).__name__, e)
sys.stdout.write(json.dumps(out))
)PY";

void CloudClient::sendViaPython(bool isTest, bool writeback,
                                const QString &reason)
{
    m_lastTest = isTest;
    m_lastWriteback = writeback;
    m_lastReason = reason;

    /* 1) the helper script (idempotent, root-only) */
    QFile script(kPyScriptPath);
    if (!script.exists() ||
        script.size() < qint64(qstrlen(kPyTransport)) / 2) {
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            finishFail(QStringLiteral("python transport: cannot write %1")
                           .arg(kPyScriptPath),
                       script.errorString());
            return;
        }
        script.write(kPyTransport, qstrlen(kPyTransport));
        script.close();
        QFile::setPermissions(kPyScriptPath,
                              QFile::ReadOwner | QFile::WriteOwner |
                                  QFile::ExeOwner);
    }

    /* 2) the job (holds the key -> 0600, deleted as soon as it is read) */
    QJsonObject job;
    job.insert(QStringLiteral("api_base"), m_s.apiBase);
    job.insert(QStringLiteral("api_key"), m_s.apiKey);
    job.insert(QStringLiteral("model"), m_s.model);
    job.insert(QStringLiteral("prompt"), m_s.prompt);
    job.insert(QStringLiteral("max_tokens"), isTest ? 4 : 64);
    job.insert(QStringLiteral("timeout_s"),
               double(m_s.timeoutMs > 0 ? m_s.timeoutMs : 5000) / 1000.0);
    job.insert(QStringLiteral("insecure"), m_s.insecureTls);
    job.insert(QStringLiteral("ca_file"), caBundlePathFor(m_s));
    QString imagePath;
    if (!isTest && !m_lastJpeg.isEmpty()) {
        imagePath = QStringLiteral("/tmp/park_cloud.jpg");
        QFile img(imagePath);
        if (img.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            img.write(m_lastJpeg);
            img.close();
            job.insert(QStringLiteral("image"), imagePath);
        } else {
            imagePath.clear();
        }
    }
    QFile jf(kPyJobPath);
    if (!jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finishFail(QStringLiteral("python transport: cannot write %1")
                       .arg(kPyJobPath),
                   jf.errorString());
        return;
    }
    jf.write(QJsonDocument(job).toJson(QJsonDocument::Compact));
    jf.close();
    QFile::setPermissions(kPyJobPath, QFile::ReadOwner | QFile::WriteOwner);

    /* 3) run it; the QTimer still bounds the whole thing (P7-04) */
    m_stats.busy = true;
    m_clock.start();
    emit started(isTest ? QStringLiteral("test") : reason);
    emit statsChanged();
    emit eventMessage(QStringLiteral(
        "cloud: %1 via python transport -> %2 (timeout %3 ms)")
                          .arg(isTest ? QStringLiteral("test")
                                      : QStringLiteral("request"))
                          .arg(QUrl(m_s.apiBase).host())
                          .arg(m_s.timeoutMs));
    qWarning("cloud: python transport (key %s)",
             qPrintable(cloud_settings_mask_key(m_s.apiKey)));

    m_py = new QProcess(this);
    m_py->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_py, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &CloudClient::onPythonFinished);
    connect(m_py, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart || m_py == nullptr)
                    return;
                const QString why = m_py->errorString();
                m_py->deleteLater();
                m_py = nullptr;
                m_timer.stop();
                QFile::remove(kPyJobPath);   /* never leave the key behind */
                finishFail(QStringLiteral("python transport unavailable"), why);
            });
    const QString py = pythonPath();
    qWarning("cloud: python transport using %s", qPrintable(py));
    m_py->start(py, QStringList() << kPyScriptPath << kPyJobPath);
    m_timer.start(m_s.timeoutMs + 1000);   /* interpreter startup allowance */
}

void CloudClient::onPythonFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_py == nullptr)
        return;                          /* already handled by errorOccurred */
    /* the script always prints JSON; its exit code only mirrors ok/failed */
    Q_UNUSED(exitCode);
    const QString out = QString::fromLocal8Bit(m_py->readAll()).trimmed();
    m_py->deleteLater();
    m_py = nullptr;
    m_timer.stop();
    QFile::remove(kPyJobPath);           /* the key must not linger in /tmp */

    if (m_timedOut) {
        m_stats.busy = false;
        updateStats(false, m_clock.elapsed(), QStringLiteral("timeout"));
        emit failed(QStringLiteral("timeout"),
                    QStringLiteral("python transport killed after %1 ms")
                        .arg(m_s.timeoutMs));
        return;
    }
    if (status != QProcess::NormalExit) {
        finishFail(QStringLiteral("python transport crashed"),
                   out.right(120));
        return;
    }

    const int brace = out.indexOf(QLatin1Char('{'));
    QJsonParseError perr;
    const QJsonDocument doc =
        QJsonDocument::fromJson(brace >= 0 ? out.mid(brace).toUtf8() : QByteArray(),
                                &perr);
    const QJsonObject o = doc.object();
    const qint64 ms = m_clock.elapsed();
    m_stats.busy = false;

    if (doc.isNull() || o.isEmpty()) {
        finishFail(QStringLiteral("python transport: bad output"),
                   out.left(120));
        return;
    }
    const int httpStatus = o.value(QStringLiteral("status")).toInt();
    const QByteArray body =
        o.value(QStringLiteral("body")).toString().toUtf8();
    if (!o.value(QStringLiteral("ok")).toBool()) {
        QString detail = o.value(QStringLiteral("error")).toString();
        if (!body.isEmpty())
            detail += QStringLiteral(" | ") + QString::fromUtf8(body.left(200));
        /* an HTTP error still carries a usable body (rate limit, bad model) */
        if (httpStatus >= 200 && httpStatus < 500 && !body.isEmpty()) {
            processBody(body, httpStatus, ms);
            return;
        }
        updateStats(false, ms, httpStatus > 0 ? httpHint(httpStatus)
                                              : QStringLiteral("python transport"));
        emit failed(QStringLiteral("python transport failed"), detail);
        return;
    }
    qWarning("cloud: python transport ok in %lld ms (http %d)",
             static_cast<long long>(ms), httpStatus);
    processBody(body, httpStatus, ms);
}

void CloudClient::finishFail(const QString &reason, const QString &detail)
{
    m_stats.busy = false;
    updateStats(false, 0, reason);
    emit failed(reason, detail);
}

void CloudClient::updateStats(bool ok, qint64 ms, const QString &err)
{
    if (ok)
        m_stats.ok++;
    else
        m_stats.fail++;
    m_stats.lastMs = ms;
    if (ms > 0)
        m_stats.totalMs += ms;
    m_stats.lastError = err;
    emit eventMessage(QStringLiteral("cloud: failed (%1) %2")
                          .arg(err)
                          .arg(ms > 0 ? QStringLiteral("%1 ms").arg(ms)
                                      : QString()));
    emit statsChanged();
}

/* ------------------------------------------------------------------ */
/* tolerant response parsing (P7-05)                                   */

bool CloudClient::extractContent(const QByteArray &body, QString *out,
                                 QString *err)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err != nullptr)
            *err = QStringLiteral("bad response envelope");
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("error"))) {
        const QJsonValue ev = root.value(QStringLiteral("error"));
        QString msg = ev.toObject().value(QStringLiteral("message")).toString();
        if (msg.isEmpty())
            msg = QStringLiteral("api error");
        QString hint = QStringLiteral("api error: %1").arg(msg.left(80));
        /* The most common operator mistake when several providers are set up:
         * a valid key from provider A pasted while api_base points at B (the
         * server answers with its own wording, e.g. DashScope's "incorrect api
         * key provided"). Say so explicitly instead of leaving a dead end. */
        const QString low = msg.toLower();
        if (low.contains(QLatin1String("api key")) ||
            low.contains(QLatin1String("api-key")) ||
            low.contains(QLatin1String("authentication")) ||
            low.contains(QLatin1String("unauthorized"))) {
            hint += QStringLiteral(
                " | this key does not match the provider at the configured "
                "api_base - check api_key for that provider");
        }
        if (err != nullptr)
            *err = hint;
        return false;
    }
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        if (err != nullptr)
            *err = QStringLiteral("no choices in response");
        return false;
    }
    const QJsonObject msg = choices.at(0).toObject()
                                .value(QStringLiteral("message")).toObject();
    QString text = msg.value(QStringLiteral("content")).toString();
    if (text.isEmpty()) {
        if (err != nullptr)
            *err = QStringLiteral("empty content");
        return false;
    }
    /* strip ```json ... ``` fences and surrounding prose */
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        const int nl = text.indexOf(QLatin1Char('\n'));
        if (nl > 0)
            text = text.mid(nl + 1);
        const int end = text.lastIndexOf(QStringLiteral("```"));
        if (end >= 0)
            text = text.left(end);
    }
    if (out != nullptr)
        *out = text.trimmed();
    return true;
}

bool CloudClient::parsePlateJson(const QString &content, QString *plate,
                                 double *conf, QString *err)
{
    /* locate the first balanced {...} object, ignoring braces inside strings */
    const int start = content.indexOf(QLatin1Char('{'));
    if (start < 0) {
        if (err != nullptr)
            *err = QStringLiteral("no json object in answer");
        return false;
    }
    int depth = 0;
    bool inStr = false, esc = false;
    int end = -1;
    for (int i = start; i < content.size(); ++i) {
        const QChar c = content.at(i);
        if (inStr) {
            if (esc)
                esc = false;
            else if (c == QLatin1Char('\\'))
                esc = true;
            else if (c == QLatin1Char('"'))
                inStr = false;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inStr = true;
        } else if (c == QLatin1Char('{')) {
            depth++;
        } else if (c == QLatin1Char('}')) {
            depth--;
            if (depth == 0) {
                end = i;
                break;
            }
        }
    }
    if (end < 0) {
        if (err != nullptr)
            *err = QStringLiteral("truncated json in answer");
        return false;
    }

    QJsonParseError pe;
    const QJsonDocument doc =
        QJsonDocument::fromJson(content.mid(start, end - start + 1).toUtf8(),
                                &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err != nullptr)
            *err = QStringLiteral("json parse error: %1").arg(pe.errorString());
        return false;
    }
    const QJsonObject o = doc.object();
    QString p = o.value(QStringLiteral("plate")).toString();
    p.remove(QLatin1Char(' '));
    p.remove(QLatin1Char('\n'));
    p.remove(QLatin1Char('\r'));
    p.remove(QLatin1Char('"'));
    p = truncateUtf8(p, 15);

    double c = 0.0;
    const QJsonValue cv = o.value(QStringLiteral("confidence"));
    if (cv.isDouble())
        c = cv.toDouble();
    else if (cv.isString())
        c = cv.toString().toDouble();
    if (c > 1.0)
        c = 1.0;                    /* some models answer 0..100 */
    if (c < 0.0)
        c = 0.0;

    if (plate != nullptr)
        *plate = p;
    if (conf != nullptr)
        *conf = c;
    return true;
}

/* UTF-8 safe truncation for park_shm's char plate[16] (15 bytes + NUL). */
QString CloudClient::truncateUtf8(const QString &s, int maxBytes)
{
    QString out;
    int bytes = 0;
    for (int i = 0; i < s.size(); ++i) {
        const QString ch = s.mid(i, 1);
        const int n = ch.toUtf8().size();
        if (bytes + n > maxBytes)
            break;
        out += ch;
        bytes += n;
    }
    return out;
}

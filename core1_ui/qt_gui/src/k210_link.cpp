#include "k210_link.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <algorithm>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

/* K210 camera mount orientation fix (2026-09-10, round 5):
 * user mounts the K210 UPRIGHT (no physical 180 turn), which flips the raw
 * frame by 180 vs the previous upside-down mount -> the correction becomes a
 * HORIZONTAL mirror only (mirrored true,false).
 * g_orientMarker lets you verify which build is deployed:
 *   strings /opt/park_ui/park_ui | grep K210-ORIENT   -> K210-ORIENT-HMIRROR
 * File demo mode is untouched (checked in publishJpeg). Pure ASCII only. */
#define K210_VIEW_HMIRROR 1
#if defined(K210_VIEW_HMIRROR)
const char g_orientMarker[] __attribute__((used)) = "K210-ORIENT-HMIRROR";
#else
const char g_orientMarker[] __attribute__((used)) = "K210-ORIENT-NONE";
#endif

/* ============================ Worker ============================ */
/* Runs in its own QThread. Owns the fd, the two frame parsers and the
 * JPEG decoder; publishes only the newest decoded QImage + signals. */

class K210LinkWorker : public QObject
{
    Q_OBJECT
public:
    explicit K210LinkWorker(QObject *parent = nullptr) : QObject(parent) {}

    QString dev = "/dev/ttyACM0";
    int baud = 115200;
    QString mode = "auto"; /* auto | text | binary | file | none */
    QString filePath;
    volatile bool stopFlag = false;

    void run();
public slots:
    void startRun() { run(); }

    bool takeFrame(QImage *out)
    {
        QMutexLocker lk(&m_mutex);
        if (!m_newFrame)
            return false;
        *out = m_latest;
        m_newFrame = false;
        return true;
    }

    void setUp(bool up)
    {
        if (m_up != up)
        {
            m_up = up;
            emit linkUp(up);
        }
    }
    bool isUp() const { return m_up; }

signals:
    void linkUp(bool up);
    void recogResult(const QString &plate, double confidence, int source);
    void recogFailed(const QString &reason);
    void snapshotCaptured(int sizeBytes);
    void busyChanged(bool busy);

private:
    void feed(const QByteArray &bytes);
    void feedText(const QByteArray &bytes);
    void feedBinary(const QByteArray &bytes);
    void processBinary(const QByteArray &frame); /* type..payload, no header/crc */
    void finishPreview(const QByteArray &jpeg, int purpose);
    void runFileMode();
    bool openSerial();
    void closeSerial();

    /* --- text parser state --- */
    QByteArray m_lineBuf;
    QMap<int, QByteArray> m_chunks; /* offset -> b64 chunk */

    /* --- binary parser state --- */
    QByteArray m_rxBuf;
    QMap<quint16, QByteArray> m_binChunks; /* seq -> payload chunk */
    quint8 m_busy = 0;

    /* --- decoded frame publish --- */
    QMutex m_mutex;
    QImage m_latest;
    bool m_newFrame = false;

    /* --- fd --- */
    int m_fd = -1;
    bool m_up = false;

    void publishJpeg(const QByteArray &jpeg)
    {
        QImage img;
        if (!img.loadFromData(jpeg, "JPEG") || img.isNull())
            return;
        /* 2026-09-10 (round 5): K210 mounted upright -> horizontal mirror
         * only. File demo mode stays unflipped. */
#if defined(K210_VIEW_HMIRROR)
        if (mode != "file")
            img = img.mirrored(true, false);
#endif
        QMutexLocker lk(&m_mutex);
        m_latest = img;
        m_newFrame = true;
    }
};

/* CRC16 XMODEM (poly 0x1021, init 0, no reflection, no final xor),
 * per docs/protocols.md section 0. */
static quint16 crc16_xmodem(const quint8 *data, int len)
{
    static quint16 table[256];
    static bool tableReady = false;
    if (!tableReady)
    {
        for (int i = 0; i < 256; ++i)
        {
            quint16 crc = quint16(i) << 8;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021)
                                     : quint16(crc << 1);
            table[i] = crc;
        }
        tableReady = true;
    }
    quint16 crc = 0x0000;
    for (int i = 0; i < len; ++i)
        crc = quint16((crc << 8) ^ table[quint8((crc >> 8) ^ data[i])]);
    return crc;
}

static speed_t baudConstant(int baud)
{
    switch (baud)
    {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 500000:
        return B500000;
    case 921600:
        return B921600;
#ifdef B1500000
    case 1500000:
        return B1500000;
#endif
    default:
        return B115200;
    }
}

bool K210LinkWorker::openSerial()
{
    m_fd = ::open(dev.toLocal8Bit().constData(),
                  O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0)
        return false;

    termios t;
    if (::tcgetattr(m_fd, &t) != 0)
    {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    t.c_cflag &= ~tcflag_t(CSIZE | PARENB | CSTOPB);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_iflag &= ~tcflag_t(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                           IGNCR | ICRNL | IXON);
    t.c_lflag &= ~tcflag_t(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_oflag &= ~tcflag_t(OPOST);
    const speed_t sp = baudConstant(baud);
    ::cfsetispeed(&t, sp);
    ::cfsetospeed(&t, sp);
    if (::tcsetattr(m_fd, TCSANOW, &t) != 0)
    {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    ::tcflush(m_fd, TCIOFLUSH);
    return true;
}

void K210LinkWorker::closeSerial()
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
}

void K210LinkWorker::run()
{
    if (mode == "none")
    {
        setUp(false);
        return;
    }
    if (mode == "file")
    {
        runFileMode();
        setUp(false);
        return;
    }

    int errStreak = 0;
    while (!stopFlag)
    {
        if (m_fd < 0)
        {
            if (openSerial())
            {
                errStreak = 0;
                setUp(true);
            }
            else
            {
                setUp(false);
                /* device may appear later (USB CDC hot-plug) */
                for (int i = 0; i < 20 && !stopFlag; ++i)
                    QThread::msleep(100);
                continue;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_fd, &rfds);
        timeval tv = {0, 100 * 1000}; /* 100 ms */
        int r = ::select(m_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            closeSerial();
            setUp(false);
            continue;
        }
        if (r == 0)
            continue; /* idle; link freshness judged by data */

        quint8 buf[4096];
        int n = int(::read(m_fd, buf, sizeof(buf)));
        if (n > 0)
        {
            errStreak = 0;
            feed(QByteArray(reinterpret_cast<const char *>(buf), n));
        }
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            if (++errStreak > 5)
            {
                closeSerial();
                setUp(false);
            }
        }
        else if (n == 0)
        {
            if (++errStreak > 5)
            { /* CDC detached */
                closeSerial();
                setUp(false);
            }
        }
    }
    closeSerial();
    setUp(false);
}

void K210LinkWorker::runFileMode()
{
    QStringList files;
    QFileInfo fi(filePath);
    if (fi.isDir())
    {
        const QFileInfoList entries =
            QDir(filePath).entryInfoList(QStringList() << "*.jpg" << "*.jpeg"
                                                       << "*.png",
                                         QDir::Files, QDir::Name);
        for (const QFileInfo &e : entries)
            files << e.absoluteFilePath();
    }
    else if (fi.isFile())
    {
        files << fi.absoluteFilePath();
    }
    if (files.isEmpty())
    {
        emit recogFailed(QString("no image files under %1").arg(filePath));
        return;
    }

    int idx = 0;
    setUp(true);
    while (!stopFlag)
    {
        QFile f(files.at(idx));
        if (f.open(QIODevice::ReadOnly))
        {
            publishJpeg(f.readAll());
            f.close();
        }
        idx = (idx + 1) % files.size();
        for (int i = 0; i < 2 && !stopFlag; ++i) /* ~5 fps */
            QThread::msleep(100);
    }
}

void K210LinkWorker::feed(const QByteArray &bytes)
{
    if (mode != "binary")
        feedText(bytes);
    if (mode != "text")
        feedBinary(bytes);
}

/* ---- text channel: "K2:IMG:<off>:<b64>" ... "K2:END:<b64len>" ---- */
void K210LinkWorker::feedText(const QByteArray &bytes)
{
    m_lineBuf += bytes;
    int nl;
    while ((nl = m_lineBuf.indexOf('\n')) >= 0)
    {
        QByteArray line = m_lineBuf.left(nl);
        m_lineBuf.remove(0, nl + 1);
        line = line.trimmed();
        if (line.startsWith("K2:IMG:"))
        {
            const QByteArray rest = line.mid(7);
            const int colon = rest.indexOf(':');
            if (colon <= 0)
                continue;
            bool okOff = false;
            const int off = rest.left(colon).toInt(&okOff);
            const QByteArray b64 = rest.mid(colon + 1);
            if (okOff && !b64.isEmpty())
                m_chunks[off] = b64;
        }
        else if (line.startsWith("K2:END:"))
        {
            bool okLen = false;
            const int want = line.mid(7).toInt(&okLen);
            QByteArray b64all;
            if (okLen && !m_chunks.isEmpty())
            {
                QList<int> keys = m_chunks.keys();
                std::sort(keys.begin(), keys.end());
                for (int k : keys)
                    b64all += m_chunks[k];
            }
            if (okLen && !b64all.isEmpty() && b64all.size() == want)
            {
                publishJpeg(QByteArray::fromBase64(b64all));
            }
            m_chunks.clear();
        }
        else if (line.startsWith("K2:OK:"))
        { /* continuous-recognition result (console uplink), 0xC2 semantics */
            const QJsonDocument doc = QJsonDocument::fromJson(
                line.mid(6).toUtf8());
            if (doc.isObject())
            {
                const QJsonObject o = doc.object();
                emit recogResult(o.value("plate").toString(),
                                 o.value("confidence").toDouble(), 0);
            }
        }
        else if (line.startsWith("K2:NG:"))
        { /* recognition failed (console uplink), 0xC3 semantics */
            emit recogFailed(line.mid(6).trimmed());
        }
    }
    if (m_lineBuf.size() > 64 * 1024)
        m_lineBuf.clear();
}

/* ---- binary channel: AA 55 | type | seq(2LE) | len(2LE) | payload | crc(2LE) ---- */
void K210LinkWorker::feedBinary(const QByteArray &bytes)
{
    m_rxBuf += bytes;
    while (true)
    {
        /* resync on header */
        int pos = 0;
        while (pos + 1 < m_rxBuf.size() &&
               !(quint8(m_rxBuf.at(pos)) == 0xAA && quint8(m_rxBuf.at(pos + 1)) == 0x55))
            ++pos;
        if (pos > 0)
            m_rxBuf.remove(0, pos);
        if (m_rxBuf.size() < 8)
            return;
        if (quint8(m_rxBuf.at(0)) != 0xAA || quint8(m_rxBuf.at(1)) != 0x55)
        {
            if (m_rxBuf.size() < 2)
                return;
            m_rxBuf.remove(0, 1);
            continue;
        }
        const quint8 type = quint8(m_rxBuf.at(2));
        const int len = int(quint8(m_rxBuf.at(6))) |
                        (int(quint8(m_rxBuf.at(7))) << 8);
        if (len > 1024)
        { /* per protocols.md: UART payload <= 1KB */
            m_rxBuf.remove(0, 1);
            continue;
        }
        const int need = 8 + len + 2;
        if (m_rxBuf.size() < need)
            return;

        const QByteArray body = m_rxBuf.mid(2, 6 + len); /* type..payload */
        const quint16 crcGot = quint16(quint8(m_rxBuf.at(8 + len))) |
                               (quint16(quint8(m_rxBuf.at(9 + len))) << 8);
        if (crc16_xmodem(reinterpret_cast<const quint8 *>(body.constData()),
                         body.size()) == crcGot)
        {
            processBinary(m_rxBuf.mid(2, 6 + len));
            m_rxBuf.remove(0, need);
        }
        else
        {
            m_rxBuf.remove(0, 1); /* bad CRC: resync 1 byte */
        }
    }
}

void K210LinkWorker::processBinary(const QByteArray &body)
{
    const quint8 type = quint8(body.at(0));
    const quint16 seq = quint16(quint8(body.at(1))) |
                        (quint16(quint8(body.at(2))) << 8);
    const QByteArray payload = body.mid(6);

    switch (type)
    {
    case 0x01: /* preview JPEG chunk */
        m_binChunks[seq] = payload;
        break;
    case 0x02:
    { /* end of frame: 4B total(LE) + 1B purpose */
        quint32 total = 0;
        if (payload.size() >= 4)
            total = quint32(quint8(payload.at(0))) |
                    (quint32(quint8(payload.at(1))) << 8) |
                    (quint32(quint8(payload.at(2))) << 16) |
                    (quint32(quint8(payload.at(3))) << 24);
        const int purpose = payload.size() >= 5 ? payload.at(4) : 0;
        QByteArray all;
        QList<quint16> keys = m_binChunks.keys();
        std::sort(keys.begin(), keys.end());
        for (quint16 k : keys)
            all += m_binChunks[k];
        m_binChunks.clear();
        if (total == 0 || all.size() == int(total))
        {
            finishPreview(all, purpose);
        }
        break;
    }
    case 0xC2:
    { /* result JSON */
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
        {
            emit recogFailed(QString("bad result json: %1").arg(err.errorString()));
            break;
        }
        const QJsonObject o = doc.object();
        emit recogResult(o.value("plate").toString(),
                         o.value("confidence").toDouble(),
                         0); /* edge result, source=0 (D2) */
        break;
    }
    case 0xC3:
    { /* failure JSON */
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        QString reason = "recognize failed";
        if (doc.isObject())
            reason = doc.object().value("error").toString(reason);
        emit recogFailed(reason);
        break;
    }
    case 0x7E:
    { /* heartbeat, bit0 busy */
        const quint8 busy = payload.isEmpty() ? 0
                                              : quint8(payload.at(0)) & 0x01;
        if (busy != m_busy)
        { /* edge only (D1): no per-frame spam */
            m_busy = busy;
            emit busyChanged(m_busy != 0);
        }
        break;
    }
    default:
        break; /* unknown type: counted, ignored */
    }
}

void K210LinkWorker::finishPreview(const QByteArray &jpeg, int purpose)
{
    if (purpose == 1)
    {
        /* recognition snapshot, reserved for the cloud fallback module (step 7) */
        emit snapshotCaptured(jpeg.size());
        return;
    }
    publishJpeg(jpeg);
}

/* ============================ facade ============================ */

K210Link::K210Link(QObject *parent)
    : QObject(parent)
{
    m_worker = new K210LinkWorker();
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    /* signal-to-signal forward, queued into the UI thread automatically */
    connect(m_worker, &K210LinkWorker::linkUp, this, [this](bool up)
            {
        m_up = up;
        emit linkUp(up); });
    connect(m_worker, &K210LinkWorker::recogResult,
            this, &K210Link::recogResult);
    connect(m_worker, &K210LinkWorker::recogFailed,
            this, &K210Link::recogFailed);
    connect(m_worker, &K210LinkWorker::snapshotCaptured,
            this, &K210Link::snapshotCaptured);
    connect(m_worker, &K210LinkWorker::busyChanged,
            this, &K210Link::busyChanged);

    m_thread.setObjectName("k210_link");
    m_thread.start();
}

K210Link::~K210Link()
{
    stop();
}

void K210Link::start(const QString &dev, int baud, const QString &mode,
                     const QString &filePath)
{
    if (m_started)
        return;
    m_started = true;
    m_worker->dev = dev;
    m_worker->baud = baud;
    m_worker->mode = mode;
    m_worker->filePath = filePath;
    QMetaObject::invokeMethod(m_worker, "startRun", Qt::QueuedConnection);
}

void K210Link::stop()
{
    if (m_worker)
    {
        m_worker->stopFlag = true;
        m_thread.quit();
        m_thread.wait(2000);
        m_worker = nullptr;
    }
}

bool K210Link::takeFrame(QImage *out)
{
    return m_worker ? m_worker->takeFrame(out) : false;
}

#include "k210_link.moc"

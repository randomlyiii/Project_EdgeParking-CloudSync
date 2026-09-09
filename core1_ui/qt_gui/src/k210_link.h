#ifndef K210_LINK_H
#define K210_LINK_H
/* K210Link - in-process receiver of the K210 preview/result stream.
 *
 * Supports the two transport variants documented in docs/protocols.md:
 *   - text  : console/USB CDC, lines "K2:IMG:<off>:<b64>" ... "K2:END:<len>"
 *             (the currently verified zero-wire link, k210_fw/main.py LINK=console)
 *   - binary: formal frame format of docs/protocols.md section 0/2 over a data
 *             UART: AA 55 | type | seq(2LE) | len(2LE) | payload | crc16(2LE)
 *             types used here: 0x01 preview chunk, 0x02 end-of-frame
 *             (4B total len LE + 1B purpose), 0xC2 result JSON, 0xC3 fail JSON,
 *             0x7E heartbeat (bit0 busy)
 * Auto mode runs both parsers on the same byte stream, so the app works
 * before and after the binary UART link replaces the console link.
 * A file mode feeds a JPEG file/directory cyclically for development.
 *
 * JPEG soft-decode (QImage::loadFromData) happens in the worker thread;
 * the UI thread pulls the latest frame via takeFrame() (drop-old policy).
 */
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QThread>
#include <QMap>

class K210LinkWorker;

class K210Link : public QObject
{
    Q_OBJECT
public:
    explicit K210Link(QObject *parent = nullptr);
    ~K210Link() override;

    void start(const QString &dev, int baud, const QString &mode,
               const QString &filePath);
    void stop();

    /* Returns true and moves the newest undisplayed frame into *out. */
    bool takeFrame(QImage *out);
    bool isUp() const { return m_up; }

signals:
    void linkUp(bool up);
    /* 0xC2 result; source 0 = edge (K210 direct), 1 = cloud fallback */
    void recogResult(const QString &plate, double confidence, int source);
    void recogFailed(const QString &reason); /* 0xC3 */
    void snapshotCaptured(int sizeBytes);    /* 0x02 purpose=1 */
    /* 0x7E heartbeat payload bit0 (busy) edge-triggered notify */
    void busyChanged(bool busy);

private:
    QThread m_thread;
    K210LinkWorker *m_worker = nullptr;
    volatile bool m_up = false;
    bool m_started = false;
};

#endif /* K210_LINK_H */

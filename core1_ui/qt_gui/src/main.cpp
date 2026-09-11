#include <QApplication>
#include <QCommandLineParser>
#include <QFont>

#include "ipc_reader.h"
#include "ipc_writer.h"
#include "k210_link.h"
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("park_ui");
    QApplication::setOrganizationName("EdgeParking");

    /* CJK-capable default family; linuxfb resolves fonts via QT_QPA_FONTDIR */
    QFont f("Noto Sans CJK SC");
    QApplication::setFont(f);

    QCommandLineParser cli;
    cli.setApplicationDescription(
        "park_ui - parking lot UI (PhaseMd/11). Qt 5.12.x, linuxfb target.");
    cli.addHelpOption();
    /* k210 link */
    QCommandLineOption optDev(QStringList() << "d" << "dev",
                              "K210 serial device (default $PARK_UI_TTY or "
                              "/dev/ttyACM0)", "dev");
    QCommandLineOption optBaud(QStringList() << "b" << "baud",
                               "serial baud (default 115200)", "baud");
    QCommandLineOption optMode(QStringList() << "m" << "mode",
                               "link mode: auto|text|binary|file|none "
                               "(default auto)", "mode");
    QCommandLineOption optFile(QStringList() << "f" << "file",
                               "image file or directory for --mode file",
                               "path");
    QCommandLineOption optDemo("demo",
                               "demo mode: on|off|auto (default auto = "
                               "simulate when /park_shm absent)", "demo");
    QCommandLineOption optEvt("eventfd",
                              "pre-opened eventfd number for IPC state "
                              "events (P6-02), default -1 = poll shm", "fd");
    cli.addOptions({optDev, optBaud, optMode, optFile, optDemo, optEvt});
    cli.process(app);

    const QString dev = cli.value(optDev).isEmpty()
        ? qEnvironmentVariable("PARK_UI_TTY", "/dev/ttyACM0")
        : cli.value(optDev);
    const int baud = cli.value(optBaud).toInt() > 0
        ? cli.value(optBaud).toInt() : 115200;
    QString mode = cli.value(optMode).toLower();
    if (mode.isEmpty())
        mode = "auto";
    const QString file = cli.value(optFile);
    const int evtFd = cli.value(optEvt).toInt();

    IpcReader::DemoMode demoMode = IpcReader::DemoAuto;
    const QString demo = cli.value(optDemo).toLower();
    if (demo == "on")
        demoMode = IpcReader::DemoForce;
    else if (demo == "off")
        demoMode = IpcReader::DemoOff;

    qRegisterMetaType<IpcSnapshot>("IpcSnapshot");

    K210Link link;
    MainWindow win;
    win.setLink(&link);

    IpcReader ipc;
    win.setIpc(&ipc);

    IpcWriter writer;
    QObject::connect(&ipc, &IpcReader::snapshotChanged,
                     &win, &MainWindow::onSnapshot);
    QObject::connect(&ipc, &IpcReader::eventMessage,
                     &win, &MainWindow::pushEvent);
    QObject::connect(&ipc, &IpcReader::platePopup,
                     &win, &MainWindow::showPlatePopup);
    QObject::connect(&link, &K210Link::linkUp,
                     &win, &MainWindow::onLinkUp);
    QObject::connect(&link, &K210Link::recogResult,
                     &win, &MainWindow::onRecogResult);
    QObject::connect(&link, &K210Link::recogFailed,
                     &win, &MainWindow::onRecogFailed);
    QObject::connect(&link, &K210Link::busyChanged,
                     &win, &MainWindow::onK210Busy);
    /* Core1 business write-end: K210 results -> shm, UI gate hotkeys -> pulse */
    QObject::connect(&link, &K210Link::recogResult,
                     &writer, &IpcWriter::onRecogResult);
    QObject::connect(&link, &K210Link::recogFailed,
                     &writer, &IpcWriter::onRecogFailed);
    QObject::connect(&win, &MainWindow::gateRequested, &writer,
                     [&writer](bool open) {
                         if (open) writer.requestGateOpen();
                         else      writer.requestGateClose();
                     });
    QObject::connect(&writer, &IpcWriter::eventMessage,
                     &win, &MainWindow::pushEvent);
    QObject::connect(&writer, &IpcWriter::cloudPendingChanged,
                     &win, &MainWindow::onCloudPending);
    QObject::connect(&writer, &IpcWriter::snapshotRefreshRequested,
                     &ipc, &IpcReader::onTick);

    ipc.start(demoMode, evtFd);
    writer.start();
    link.start(dev, baud, mode, file);

    win.show();
    const int rc = app.exec();

    link.stop();
    writer.stop();
    ipc.stop();
    return rc;
}

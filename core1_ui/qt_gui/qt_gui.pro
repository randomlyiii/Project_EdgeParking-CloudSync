# park_ui - parking lot Qt UI (Core1, per PhaseMd/11 Qt spec)
# Target board: 100ASK MP157, factory rootfs ships Qt 5.12.8 (linuxfb, no GPU).
# Build (PC dev, must match board Qt version 5.12.x):
#   qmake && make
# Cross build (align with 100ASK/ST SDK, sysroot = board rootfs):
#   source <sdk-env>/environment-setup-... && qmake && make
QT       += core gui widgets
CONFIG   += c++11
TARGET    = park_ui
TEMPLATE  = app

SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/k210_link.cpp \
    src/ipc_reader.cpp \
    src/ipc_writer.cpp

HEADERS += \
    src/mainwindow.h \
    src/k210_link.h \
    src/ipc_reader.h \
    src/ipc_writer.h \
    src/park_shm.h

# shm_open() lives in librt on older glibc
unix:!macx: LIBS += -lrt

DESTDIR = bin
MOC_DIR = build
OBJECTS_DIR = build
UI_DIR = build

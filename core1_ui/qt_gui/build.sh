#!/bin/sh
# One-shot build (+ optional demo smoke test) for park_ui.
# Board / Linux host only (ASCII comments, no sudo on the board).
#
# Usage:
#   ./build.sh          # static checks + qmake + make
#   ./build.sh demo     # ...then launch the demo on linuxfb
set -e
cd "$(dirname "$0")"

# pre-build source sanity (no Qt required)
if command -v python3 >/dev/null 2>&1; then
    python3 tools/check_static.py
else
    echo "[warn] python3 not found, skipping static check"
fi

# cross-build tools are NOT on the board (board has the Qt 5.12.8 runtime
# only); build on a PC with Qt 5.12.8 + 100ASK/ST SDK, then scp bin/park_ui.
for tool in qmake make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[error] $tool not found: cross-build on PC (Qt 5.12.8 + 100ASK SDK), then scp bin/park_ui to the board"
        exit 1
    fi
done

# Recreate qmake outputs so stale moc files cannot survive source transfers.
rm -f Makefile build/k210_link.moc build/moc_*.cpp
qmake && make -j4

echo "[ok] built: bin/park_ui"

if [ "$1" = "demo" ]; then
    export QT_QPA_PLATFORM=linuxfb:nocursor=1
    export QT_QPA_FONTDIR=/usr/share/fonts
    export LD_LIBRARY_PATH=/usr/lib
    echo "[run] demo mode (Ctrl-C to exit)"
    exec ./bin/park_ui --mode file -f ./testimgs --demo on
fi
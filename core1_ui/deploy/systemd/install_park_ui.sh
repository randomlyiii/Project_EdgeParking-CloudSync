#!/bin/sh
# install_park_ui.sh - deploy park_ui onto the MP157 board (root).
#
# Usage (run ON THE BOARD as root, after scp'ing this whole deploy/ tree and
# the built bin/park_ui):
#   sh deploy/systemd/install_park_ui.sh
#
# What it does:
#   1. copies park_ui to /opt/park_ui/park_ui (chmod +x)
#   2. installs park-ui.service and enables it
#   3. prints the DISABLE-DEFAULT-PAGE checklist (vendor demo/fbcon owner)
#      - run 'ps -ef | grep -iE "qt|demo|eglfs"' to find what draws the LCD
#        "default page"; stop/disable that init script or systemd unit, then
#        'systemctl restart park-ui'
#
# ASCII only (board rule). Run as root, no sudo on board.
set -e

BIN=/opt/park_ui/park_ui
SRC=${SRC:-./bin/park_ui}          # override with the actual built binary path

echo "[1/3] install binary"
mkdir -p /opt/park_ui
if [ -f "$SRC" ]; then
    cp "$SRC" "$BIN"
    chmod +x "$BIN"
else
    echo "[warn] $SRC not found - copy park_ui to /opt/park_ui/ manually"
fi

echo "[2/3] install systemd unit"
cp park-ui.service /etc/systemd/system/park-ui.service
systemctl daemon-reload
systemctl enable park-ui.service

echo "[3/3] notes"
echo "  - /etc/park-ui.env (optional): extra env, e.g. PARK_UI_TTY=/dev/ttyACM0"
echo "  - K210 console text link: device /dev/ttyACM0, --mode auto parses it"
echo "  - unit kills the vendor desktop (mxapp2/eglfs) in ExecStartPre so the"
echo "    LCD goes straight to park_ui; no binary rename needed"
echo "  - start now: systemctl start park-ui ; journalctl -u park-ui -f"

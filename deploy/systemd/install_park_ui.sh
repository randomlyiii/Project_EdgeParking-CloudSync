#!/bin/sh
# install_park_ui.sh - deploy park_ui onto the MP157 board (root).
#
# Usage (run ON THE BOARD as root, after scp'ing this whole deploy/ tree and
# the built bin/park_ui):
#   sh deploy/systemd/install_park_ui.sh
#
# What it does:
#   1. copies park_ui to /opt/park_ui/park_ui (chmod +x); falls back to
#      /root/park_ui if the repo-local ./bin/park_ui is absent
#   2. disables the vendor HMI desktop (myir.service -> mxapp2/eglfs) so the
#      LCD goes straight to park_ui and nothing respawns over it
#   3. installs and enables park-ui.service (its ExecStartPre also SIGKILLs
#      mxapp2 as belt-and-braces)
#
# ASCII only (board rule). Run as root (or sudo -i on book).
set -e

BIN=/opt/park_ui/park_ui
SRC=${SRC:-./bin/park_ui}          # fallback to /root/park_ui if missing

echo "[1/4] install binary"
if [ -f "$SRC" ]; then
    mkdir -p /opt/park_ui
    cp "$SRC" "$BIN"
elif [ -f /root/park_ui ]; then
    mkdir -p /opt/park_ui
    cp /root/park_ui "$BIN"
else
    echo "[warn] no park_ui found (./bin/park_ui or /root/park_ui) - copy it to /opt/park_ui/ manually"
fi
chmod +x "$BIN" 2>/dev/null || true

echo "[2/4] disable vendor HMI desktop (myir.service / mxapp2-eglfs)"
if systemctl list-unit-files 2>/dev/null | grep -q '^myir.service'; then
    systemctl disable --now myir.service 2>/dev/null || true
    echo "  myir.service disabled and stopped"
else
    echo "  myir.service not present (already removed?)"
fi
pkill -9 -f "mxapp2" 2>/dev/null || true

echo "[3/4] install systemd unit"
cp park-ui.service /etc/systemd/system/park-ui.service
systemctl daemon-reload
systemctl enable park-ui.service

echo "[4/4] notes"
echo "  - /etc/park-ui.env (optional): extra env, e.g. PARK_UI_TTY=/dev/ttyACM0"
echo "  - K210 console text link: device /dev/ttyACM0, --mode auto parses it"
echo "  - start now: systemctl start park-ui ; journalctl -u park-ui -f"

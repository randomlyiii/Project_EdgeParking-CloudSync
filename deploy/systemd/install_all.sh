#!/bin/sh
# install_all.sh - install the FULL EdgeParking chain on the MP157 board.
#
# Usage (run ON THE BOARD as root, after scp'ing deploy/ + the ARM binaries):
#   sh deploy/systemd/install_all.sh
#
# Boot chain (systemd ordering m4-load -> core0-bus -> park-ui):
#   m4-load.service   remoteproc start: loads M4 fw, brings can0 up (keeps the
#                     fdcan_k clock alive) and binds /dev/ttyRPMSG0
#   core0-bus.service core0_business: owns /park_shm (v3), RPMSG to M4, 7-state
#                     business state machine, config hot reload
#   park-ui.service   park_ui (Core1): Qt UI on linuxfb, reads /park_shm
#
# Optional env overrides:
#   CORE0_SRC=...    path to the core0_business binary (else auto-found)
#   UI_SRC=...       path to park_ui          (default ./bin/park_ui, /root/park_ui)
#   M4_FW=...        path to the M4 .elf      (else auto-found, copied to
#                                              /lib/firmware/m4_fw.elf)
#   INSTALL_M4=0 / INSTALL_CORE0=0 / INSTALL_UI=0   skip a component
#
# ASCII only (board rule). Root required (this board has no sudo).
set -e

DIR=$(cd "$(dirname "$0")" && pwd)          # .../deploy/systemd
CORE0_DST=/opt/core0
UI_DST=/opt/park_ui/park_ui
FW_DST=/lib/firmware/m4_fw.elf

INSTALL_M4=${INSTALL_M4:-1}
INSTALL_CORE0=${INSTALL_CORE0:-1}
INSTALL_UI=${INSTALL_UI:-1}

# first existing file among the arguments (empty output if none)
pick() {
    for p in "$@"; do
        if [ -n "$p" ] && [ -f "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

echo "[1/7] resolve sources"
CORE0_BIN=$(pick "${CORE0_SRC:-}" ./core0_business /root/core0_business \
                 /root/core0_service/core0_business) || true
CONF=$(pick ./core0.conf /root/core0.conf /root/core0_service/core0.conf \
            ./core0.conf.example /root/core0_service/core0.conf.example) || true
UI_BIN=$(pick "${UI_SRC:-}" ./bin/park_ui /root/park_ui ./park_ui) || true
LOADM4=$(pick ./tools/load_m4.sh /root/core0_service/tools/load_m4.sh \
              "$DIR/../../core0_service/tools/load_m4.sh") || true
LINKTEST=$(pick ./tools/rpmsg_link_test.py \
                /root/core0_service/tools/rpmsg_link_test.py \
                "$DIR/../../core0_service/tools/rpmsg_link_test.py") || true
M4ELF=$(pick "${M4_FW:-}" ./m4_fw.elf /root/m4_fw.elf /root/m4_fw_CM4.elf \
             /root/m4_fw/CM4/Debug/m4_fw_CM4.elf) || true

echo "  core0_business : ${CORE0_BIN:-<not found>}"
echo "  core0.conf     : ${CONF:-<not found>}"
echo "  park_ui        : ${UI_BIN:-<not found>}"
echo "  load_m4.sh     : ${LOADM4:-<not found>}"
echo "  link_test.py   : ${LINKTEST:-<not found>}"
echo "  M4 elf         : ${M4ELF:-<not found, keep existing $FW_DST>}"

echo "[2/7] install core0 ($CORE0_DST)"
mkdir -p "$CORE0_DST/tools"
if [ -n "$CORE0_BIN" ]; then
    cp "$CORE0_BIN" "$CORE0_DST/core0_business"
    chmod +x "$CORE0_DST/core0_business"
else
    echo "  [warn] core0_business not found - cross-compile it on the book first:"
    echo "         cd ~/core0_service && make CROSS_COMPILE=arm-buildroot-linux-gnueabihf- core0_business"
    INSTALL_CORE0=0
fi
if [ -n "$CONF" ]; then
    # never clobber a live config (it is the operator's source of truth)
    if [ -f "$CORE0_DST/core0.conf" ]; then
        echo "  keep existing $CORE0_DST/core0.conf"
    else
        cp "$CONF" "$CORE0_DST/core0.conf"
        echo "  installed $CORE0_DST/core0.conf"
    fi
else
    echo "  [warn] no core0.conf / core0.conf.example found"
fi
[ -n "$LOADM4" ]   && cp "$LOADM4"   "$CORE0_DST/tools/load_m4.sh"        && chmod +x "$CORE0_DST/tools/load_m4.sh"
[ -n "$LINKTEST" ] && cp "$LINKTEST" "$CORE0_DST/tools/rpmsg_link_test.py" && chmod +x "$CORE0_DST/tools/rpmsg_link_test.py"
[ -n "$LOADM4" ] || INSTALL_M4=0

echo "[3/7] install park_ui ($UI_DST)"
if [ -n "$UI_BIN" ]; then
    mkdir -p /opt/park_ui
    cp "$UI_BIN" "$UI_DST"
    chmod +x "$UI_DST"
else
    echo "  [warn] park_ui not found (./bin/park_ui or /root/park_ui) - build it on the book"
    INSTALL_UI=0
fi

echo "[4/7] install M4 firmware"
if [ -n "$M4ELF" ]; then
    cp "$M4ELF" "$FW_DST"
    echo "  installed $FW_DST"
elif [ -f "$FW_DST" ]; then
    echo "  keep existing $FW_DST"
else
    echo "  [warn] no M4 elf and no $FW_DST - m4-load will fail"
    echo "         scp m4_fw/CM4/Debug/m4_fw_CM4.elf root@<board>:/lib/firmware/m4_fw.elf"
    INSTALL_M4=0
fi

echo "[5/7] disable vendor HMI desktop (myir.service / mxapp2-eglfs)"
if systemctl list-unit-files 2>/dev/null | grep -q '^myir.service'; then
    # NOTE: /usr/bin/start.sh (run by myir.service) also drove the two board
    # power-rail GPIOs high. Disabling the HMI therefore also killed the onboard
    # USB hub -> the K210 never enumerated. board-power.service replicates those
    # two writes, so both must always be installed together (step [6/7]).
    systemctl disable --now myir.service 2>/dev/null || true
    echo "  myir.service disabled and stopped"
else
    echo "  myir.service not present (already removed?)"
fi
pkill -9 -f "mxapp2" 2>/dev/null || true

echo "[6/7] board power rails (USB host VBUS / onboard hub enable)"
cp "$DIR/board-power.service" /etc/systemd/system/board-power.service
systemctl enable board-power.service
systemctl start board-power.service 2>/dev/null || true
echo "  enabled board-power.service (GPIO 82/PF2 + 139/PI11 high)"

echo "[7/7] install systemd units"
# always ship park-ui.service (it is also the UI-only entry point)
cp "$DIR/park-ui.service" /etc/systemd/system/park-ui.service
systemctl daemon-reload
if [ "$INSTALL_M4" = "1" ]; then
    cp "$DIR/m4-load.service" /etc/systemd/system/m4-load.service
    systemctl enable m4-load.service
    echo "  enabled m4-load.service"
fi
if [ "$INSTALL_CORE0" = "1" ]; then
    cp "$DIR/core0-bus.service" /etc/systemd/system/core0-bus.service
    systemctl enable core0-bus.service
    echo "  enabled core0-bus.service"
fi
if [ "$INSTALL_UI" = "1" ]; then
    systemctl enable park-ui.service
    echo "  enabled park-ui.service"
else
    systemctl disable park-ui.service 2>/dev/null || true
fi
systemctl daemon-reload

echo
echo "done. boot chain: m4-load -> core0-bus -> park-ui"
echo "optional extra env for the UI: /etc/park-ui.env (e.g. PARK_UI_TTY=/dev/ttyACM0)"
echo
echo "start everything now:"
echo "  systemctl start m4-load core0-bus park-ui"
echo "check:"
echo "  systemctl status m4-load core0-bus park-ui --no-pager"
echo "  ls -l /dev/ttyRPMSG0 /dev/shm/park_shm"
echo "  journalctl -u m4-load -u core0-bus -u park-ui -n 40 --no-pager"
echo
echo "M4 bring-up is flaky; retry by hand (wait >=2 min between tries):"
echo "  systemctl restart m4-load"

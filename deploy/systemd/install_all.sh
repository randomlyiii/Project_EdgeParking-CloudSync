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

echo "[1/8] resolve sources"
CORE0_BIN=$(pick "${CORE0_SRC:-}" ./core0_business /root/core0_business \
                 /root/core0_service/core0_business) || true
CONF=$(pick ./core0.conf /root/core0.conf /root/core0_service/core0.conf \
            ./sample_core0.conf /root/core0_service/sample_core0.conf) || true
UI_BIN=$(pick "${UI_SRC:-}" ./bin/park_ui /root/park_ui ./park_ui) || true
LOADM4=$(pick ./tools/load_m4.sh /root/core0_service/tools/load_m4.sh \
              "$DIR/../../core0_service/tools/load_m4.sh") || true
LINKTEST=$(pick ./tools/rpmsg_link_test.py \
                /root/core0_service/tools/rpmsg_link_test.py \
                "$DIR/../../core0_service/tools/rpmsg_link_test.py") || true
# CubeIDE builds the M4 core as "m4_fw_CM4.elf" (inside m4_fw/CM4/Debug/).
# The DEPLOYED name stays "m4_fw.elf" (load_m4.sh/remoteproc expect it, $FW_DST).
M4ELF=$(pick "${M4_FW:-}" \
             ./m4_fw_CM4.elf ./m4_fw.elf \
             ./m4_fw/CM4/Debug/m4_fw_CM4.elf \
             /root/m4_fw_CM4.elf /root/m4_fw.elf \
             /root/m4_fw/CM4/Debug/m4_fw_CM4.elf) || true

echo "  core0_business : ${CORE0_BIN:-<not found>}"
echo "  core0.conf     : ${CONF:-<not found>}"
echo "  park_ui        : ${UI_BIN:-<not found>}"
echo "  load_m4.sh     : ${LOADM4:-<not found>}"
echo "  link_test.py   : ${LINKTEST:-<not found>}"
if [ -n "$M4ELF" ]; then
    echo "  M4 elf         : $M4ELF (will install as $FW_DST)"
elif [ -f "$FW_DST" ]; then
    echo "  M4 elf         : keep existing $FW_DST"
else
    echo "  M4 elf         : <not found> - scp m4_fw_CM4.elf to /root first"
fi

echo "[2/8] install core0 ($CORE0_DST)"
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
    echo "  [warn] no core0.conf / sample_core0.conf found"
fi
[ -n "$LOADM4" ]   && cp "$LOADM4"   "$CORE0_DST/tools/load_m4.sh"        && chmod +x "$CORE0_DST/tools/load_m4.sh"
[ -n "$LINKTEST" ] && cp "$LINKTEST" "$CORE0_DST/tools/rpmsg_link_test.py" && chmod +x "$CORE0_DST/tools/rpmsg_link_test.py"
[ -n "$LOADM4" ] || INSTALL_M4=0

echo "[3/9] install park_ui ($UI_DST)"
if [ -n "$UI_BIN" ]; then
    mkdir -p /opt/park_ui
    cp "$UI_BIN" "$UI_DST"
    chmod +x "$UI_DST"
else
    echo "  [warn] park_ui not found (./bin/park_ui or /root/park_ui) - build it on the book"
    INSTALL_UI=0
fi
# clock helper: the board has no RTC and boots in 2020, which breaks every
# HTTPS certificate -> park-clock.service runs this before park-ui (step [9/9])
cp "$DIR/set_clock.py" /opt/park_ui/set_clock.py
chmod +x /opt/park_ui/set_clock.py
echo "  installed /opt/park_ui/set_clock.py"

echo "[4/9] WiFi bring-up helper + unit (wlan0 at boot)"
# The board's only link is WiFi. park_ui raises it only when the operator taps
# CONNECT, and myir.service (which used to do it at boot) is disabled below, so
# without this step every power cycle needs the three vendor commands by hand -
# and a dead link is indistinguishable from a cloud failure on the panel.
mkdir -p /opt/park_ui/tools
cp "$DIR/wifi_up.sh" /opt/park_ui/tools/wifi_up.sh
chmod +x /opt/park_ui/tools/wifi_up.sh
cp "$DIR/wifi-up.service" /etc/systemd/system/wifi-up.service
systemctl daemon-reload
systemctl enable wifi-up.service
systemctl start wifi-up.service 2>/dev/null || true
echo "  installed /opt/park_ui/tools/wifi_up.sh + enabled wifi-up.service"

echo "[5/9] install M4 firmware"
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

echo "[6/9] disable vendor HMI desktop (myir.service / mxapp2-eglfs)"
if systemctl list-unit-files 2>/dev/null | grep -q '^myir.service'; then
    # NOTE: /usr/bin/start.sh (run by myir.service) also drove the two board
    # power-rail GPIOs high. Disabling the HMI therefore also killed the onboard
    # USB hub -> the K210 never enumerated. board-power.service replicates those
    # two writes, so both must always be installed together (step [7/9]).
    systemctl disable --now myir.service 2>/dev/null || true
    echo "  myir.service disabled and stopped"
else
    echo "  myir.service not present (already removed?)"
fi
# no pkill on this board: walk /proc and kill by comm/argv (skip nothing here,
# this script's own name does not contain mxapp2)
for d in /proc/[0-9]*; do
    [ -r "$d/cmdline" ] || continue
    if tr '\0' ' ' < "$d/cmdline" 2>/dev/null | grep -q mxapp2; then
        kill -9 "${d#/proc/}" 2>/dev/null || true
    fi
done

echo "[7/9] board power rails (USB host VBUS / onboard hub enable)"
cp "$DIR/board-power.service" /etc/systemd/system/board-power.service
systemctl enable board-power.service
systemctl start board-power.service 2>/dev/null || true
echo "  enabled board-power.service (GPIO 82/PF2 + 139/PI11 high)"

echo "[8/9] cloud fallback config (/etc/park/cloud.conf)"
# Step 7: the DeepSeek/OpenAI-compatible settings live OUTSIDE the repo (the API
# key must never be committed, P7-07). The GUI reads this file and the LCD
# settings page rewrites it atomically (keeping a .bak). Never overwrite an
# existing file here - it holds the operator's key.
install -d -m 700 /etc/park
CLOUD_EX=
for p in "$DIR/../sample_cloud.conf" "$DIR/sample_cloud.conf"; do
    if [ -f "$p" ]; then CLOUD_EX=$p; break; fi
done
if [ -f /etc/park/cloud.conf ]; then
    echo "  keep existing /etc/park/cloud.conf (key untouched)"
elif [ -n "$CLOUD_EX" ]; then
    cp "$CLOUD_EX" /etc/park/cloud.conf
    chmod 600 /etc/park/cloud.conf
    echo "  installed template /etc/park/cloud.conf (mode 600)"
    echo "  [todo] put the real key in api_key= (or export DEEPSEEK_API_KEY)"
else
    echo "  [warn] no sample_cloud.conf - create /etc/park/cloud.conf by hand"
fi

echo "[9/9] install systemd units"
# always ship park-ui.service (it is also the UI-only entry point)
cp "$DIR/park-ui.service" /etc/systemd/system/park-ui.service
# clock: no RTC on this board -> boots in 2020 -> HTTPS certs invalid
cp "$DIR/park-clock.service" /etc/systemd/system/park-clock.service
cp "$DIR/park-clock.timer" /etc/systemd/system/park-clock.timer
systemctl enable park-clock.service
systemctl start park-clock.service 2>/dev/null || true
# the timer is the second chance: park-clock runs once at boot, right after
# wifi-up - if DHCP/DNS is late it fails silently and the clock stays in 2020
systemctl enable park-clock.timer 2>/dev/null || true
systemctl start park-clock.timer 2>/dev/null || true
echo "  enabled park-clock.service + park-clock.timer (HTTP Date -> date -s, before park-ui)"
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
echo "done. boot chain: board-power -> wifi-up -> park-clock -> m4-load -> core0-bus -> park-ui"
echo "optional extra env for the UI: /etc/park-ui.env (e.g. PARK_UI_TTY=/dev/ttyACM0, PARK_UI_WIFI=wlan0)"
echo
echo "start everything now:"
echo "  systemctl start wifi-up park-clock m4-load core0-bus park-ui"
echo "check:"
echo "  date -u                      # must be the real time, not 2020"
echo "  journalctl -u park-clock -n 10 --no-pager   # set_clock: clock set to ... UTC"
echo "  journalctl -u wifi-up -n 10 --no-pager   # wifi-up: wlan0 online (ssid=...)"
echo "  systemctl status wifi-up park-clock m4-load core0-bus park-ui --no-pager"
echo "  ls -l /dev/ttyRPMSG0 /dev/shm/park_shm"
echo "  journalctl -u park-clock -u m4-load -u core0-bus -u park-ui -n 40 --no-pager"
echo
echo "re-run the WiFi bring-up after editing /etc/wpa_supplicant.conf:"
echo "  systemctl restart wifi-up"
echo
echo "M4 bring-up is flaky; retry by hand (wait >=2 min between tries):"
echo "  systemctl restart m4-load"

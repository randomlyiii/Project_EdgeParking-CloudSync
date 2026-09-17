#!/bin/sh
# EdgeParking WiFi bring-up - the vendor recipe, made idempotent for boot.
#
#   ip link set wlan0 up
#   wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf
#   udhcpc -i wlan0
#
# Why this exists (2026-09-11, user bug report "every boot needs the three
# commands by hand"): park_ui raises wlan0 only when the operator presses
# CONNECT on the LCD settings page. myir.service (the vendor desktop) used to
# bring the link up at boot, and we disable it on purpose - so afterwards
# nothing did. A link that never comes up looks exactly like a "cloud failure"
# on the panel, hence wifi-up.service runs this before park-clock and park-ui.
#
# Configuration (environment, normally via /etc/park-ui.env):
#   PARK_UI_WIFI=wlan0                      interface
#   PARK_WPA_CONF=/etc/wpa_supplicant.conf  wpa_supplicant config
#
# The WiFi passphrase is NEVER printed and never passed on a command line: only
# the interface, the DHCP outcome and the SSID reach the journal. The leased
# address is deliberately not reported either (panel privacy rule).
#
# Time budget (2026-09-16): association <= 12 s, DHCP <= 15 s, plus ~5 s of
# fixed sleeps => the script cannot hold the boot for more than ~32 s, and this
# unit no longer sits in front of park-ui/m4-load/core0-bus anyway.
set -u

IFACE=${PARK_UI_WIFI:-wlan0}
CONF=${PARK_WPA_CONF:-/etc/wpa_supplicant.conf}

# Absolute paths only: this rootfs keeps ip/wpa_supplicant/udhcpc in /sbin,
# which a systemd unit's PATH is not guaranteed to contain.
tool() {
    for d in /sbin /usr/sbin /bin /usr/bin; do
        if [ -x "$d/$1" ]; then
            echo "$d/$1"
            return 0
        fi
    done
    echo "$1"
}

IP=$(tool ip)
WPA=$(tool wpa_supplicant)
UDH=$(tool udhcpc)
WPACLI=$(tool wpa_cli)

if [ ! -f "$CONF" ]; then
    echo "wifi-up: $CONF not found - nothing to bring up"
    exit 0
fi

if ! "$IP" link set "$IFACE" up 2>/dev/null; then
    echo "wifi-up: cannot set $IFACE up (missing interface?)"
    exit 0
fi
echo "wifi-up: $IFACE up, starting wpa_supplicant with $CONF"

# Drop a stale wpa_supplicant first (this image has no procps: no pgrep/pkill).
for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ "$pid" = "$$" ] && continue
    if cat "$p/cmdline" 2>/dev/null | tr '\0' ' ' | grep -q wpa_supplicant; then
        kill "$pid" 2>/dev/null
    fi
done
sleep 1

"$WPA" -B -D nl80211 -i "$IFACE" -c "$CONF" >/dev/null 2>&1
sleep 2

# Association gate (2026-09-16, user report "with WiFi off the board hangs at the
# network step"): with no AP in range there is NOTHING to lease, so the DHCP
# retries below would only burn boot time. Wait a bounded moment for
# wpa_supplicant to reach COMPLETED and skip DHCP entirely if it never does.
# wpa_cli is not guaranteed to exist - if it is missing we keep the old
# behaviour instead of skipping a link that might be perfectly fine.
if [ -x "$WPACLI" ]; then
    state=""
    j=1
    while [ "$j" -le 12 ]; do
        state=$("$WPACLI" -i "$IFACE" status 2>/dev/null \
                | sed -n 's/^wpa_state=//p' | head -n 1)
        [ "$state" = "COMPLETED" ] && break
        sleep 1
        j=$((j + 1))
    done
    if [ "$state" != "COMPLETED" ]; then
        echo "wifi-up: $IFACE not associated (wpa_state=${state:-unknown}) - nothing to join, skipping DHCP"
        exit 0
    fi
    echo "wifi-up: $IFACE associated (wpa_state=COMPLETED)"
fi

# DHCP: hard-bounded by a wall-clock deadline. -n/-t are not honoured by every
# udhcpc build, so the script bounds the loop itself; the board clock is wrong
# but monotonic, which is all a 15 s budget needs.
DEADLINE=$(( $(date +%s) + 15 ))
i=1
while [ "$i" -le 3 ]; do
    if [ "$(date +%s)" -ge "$DEADLINE" ]; then
        echo "wifi-up: DHCP deadline reached after $((i - 1)) attempt(s)"
        break
    fi
    if "$UDH" -i "$IFACE" -n -q -t 4 -T 2 >/dev/null 2>&1; then
        break
    fi
    echo "wifi-up: DHCP attempt $i did not get a lease"
    i=$((i + 1))
    sleep 2
done

if "$IP" -4 addr show "$IFACE" 2>/dev/null | grep -q 'inet '; then
    ssid=$("$WPACLI" -i "$IFACE" status 2>/dev/null | sed -n 's/^ssid=//p' | head -n 1)
    echo "wifi-up: $IFACE online (ssid=$ssid)"
else
    echo "wifi-up: no IPv4 lease on $IFACE - check $CONF and the AP"
fi

# Never fail the boot: the local business loop must come up without a network.
exit 0

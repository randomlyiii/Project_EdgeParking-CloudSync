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

# DHCP: three attempts, five tries each (an AP that is slow to answer must not
# cost us the whole boot).
i=1
while [ "$i" -le 3 ]; do
    if "$UDH" -i "$IFACE" -n -q -t 5 -T 3 >/dev/null 2>&1; then
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

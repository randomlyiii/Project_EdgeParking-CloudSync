#!/bin/sh
# rk3588-eth.sh - static link to the RK3588 edge node + hostname entry.
#
# The RK3588 (netplan) owns 192.168.10.2/24 on its eth0; this script gives the
# MP157 side 192.168.10.1/24 and maps the "rk3588" hostname so park_ui can
# reach the edge hub relay (PARK_UI_K210_TCP, default rk3588:8089) without any
# IP literal in the Qt sources.
#
# Live path (2026-09-26): MP157 BUILT-IN RJ45 (eth0) <-> RK3588 eth0, both
# gigabit, direct cable. The earlier RTL8152 USB-dongle path flapped its
# carrier every few seconds and was retired - keep it only as a fallback and
# set IFACE=eth1 in that case.
#
# RK side gotcha (same day): GNOME NetworkManager had leftover profiles that
# grabbed eth0 via DHCP (static 10.2 never applied, manual adds got flushed).
# Fix on RK: delete the NM profiles for eth0 AND pin
# /etc/NetworkManager/conf.d/99-eth0-unmanaged.conf with
#   [keyfile]
#   unmanaged-devices=interface-name:eth0
# (full story: the RK3588 bring-up notes in the repo, section 9).
#
# Idempotent, safe to re-run. The cloud path stays on WiFi (wifi_up.sh); this
# link carries only the local edge traffic. ASCII only (board rule).
set -e

IFACE=eth0
ADDR=192.168.10.1/24
PEER_IP=192.168.10.2
PEER_NAME=rk3588

ip link set "$IFACE" up
ip addr replace "$ADDR" dev "$IFACE"

# hostname entry (idempotent): drop any stale rk3588 alias, re-add the mapping
if ! grep -q "^${PEER_IP}[[:space:]]${PEER_NAME}\$" /etc/hosts 2>/dev/null; then
    grep -v "[[:space:]]${PEER_NAME}\$" /etc/hosts > /etc/hosts.tmp 2>/dev/null || true
    printf '%s\t%s\n' "$PEER_IP" "$PEER_NAME" >> /etc/hosts.tmp
    cat /etc/hosts.tmp > /etc/hosts
    rm -f /etc/hosts.tmp
fi

echo "rk3588-eth: ${IFACE}=${ADDR}, ${PEER_NAME} -> ${PEER_IP}"
exit 0

#!/bin/sh
# run_board.sh - run park_ui manually on the MP157 board (root, no sudo).
#
# Sets the linuxfb env the board needs (Qt 5.12.8 runtime in /usr/lib) and
# launches with K210 text-console link in auto mode.
#
# Usage:
#   sh run_board.sh               # K210 video + demo slots (auto shm/demo)
#   sh run_board.sh --mode file -f /root/test.jpg   # local image preview
#
# If the LCD still shows the vendor "default page" instead of this UI, some
# other process owns the display (vendor Qt demo / fbcon / eglfs). Find it:
#   ps -ef | grep -iE 'qt|demo|eglfs|weston'
#   ls /etc/init.d/
# and stop/disable it, then rerun this script.
#
# ASCII only (board rule).
export QT_QPA_PLATFORM=linuxfb:nocursor=1
export QT_QPA_FONTDIR=/usr/share/fonts
export LD_LIBRARY_PATH=/usr/lib
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms

BIN=$(dirname "$0")/bin/park_ui
[ -x "$BIN" ] || BIN=/opt/park_ui/park_ui

exec "$BIN" --mode auto "$@"

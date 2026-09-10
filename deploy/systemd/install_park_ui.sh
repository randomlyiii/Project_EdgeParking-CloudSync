#!/bin/sh
# install_park_ui.sh - UI-only install (kept for backward compatibility).
#
# This is now a thin wrapper around install_all.sh with the core0 and M4
# units disabled, so the old command still works:
#     sh deploy/systemd/install_park_ui.sh
#
# For the FULL chain (M4 + core0 + park_ui) use:
#     sh deploy/systemd/install_all.sh
#
# ASCII only (board rule). Root required (this board has no sudo).
set -e

DIR=$(cd "$(dirname "$0")" && pwd)

INSTALL_M4=0
INSTALL_CORE0=0
export INSTALL_M4 INSTALL_CORE0

echo "note: UI-only install (backward compatible)."
echo "      for M4 + core0 + park_ui use: sh $DIR/install_all.sh"
exec sh "$DIR/install_all.sh" "$@"

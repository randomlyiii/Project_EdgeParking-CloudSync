#!/bin/sh
# Cross-build park_ui for the board (must match the board's Qt 5.12.8).
#
# Run this ON THE BOOK (the Linux build machine), from anywhere:
#     sh tools/build_arm.sh
#     sh tools/build_arm.sh /path/to/arm-buildroot-linux-gnueabihf_sdk-buildroot
#
# Why a script instead of "qmake && make":
#   1) This Buildroot SDK's Qt mkspec bakes in the ABSOLUTE path where the SDK
#      was originally built (/home/book/stm32mp157/ST-Buildroot/output/host).
#      qmake runs a compiler probe whenever .qmake.stash is missing, and with
#      the baked path gone it dies with
#          Project ERROR: Cannot run target compiler '.../ST-Buildroot/.../g++'
#      Fix: point that legacy path at the current SDK with a symlink (no sudo
#      needed, nothing outside /home/book is touched). Every baked path -
#      compiler, sysroot, ar/ld - resolves again.
#   2) qmake writes --sysroot= pointing at the wrong sysroot; the Makefile is
#      rewritten afterwards.
#   3) New source files are not picked up by an incremental make, hence touch.
# ASCII only (board rule).
set -e

SDK=${1:-${SDK:-/home/book/100ask_stm32mp157_pro-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot}}
SYS=$SDK/arm-buildroot-linux-gnueabihf/sysroot
LEGACY=/home/book/stm32mp157/ST-Buildroot/output/host
QMAKE=$SDK/bin/qmake
CC=$SDK/bin/arm-buildroot-linux-gnueabihf-gcc
CXX=$SDK/bin/arm-buildroot-linux-gnueabihf-g++

fail() { echo "ERROR: $1"; exit 1; }

[ -x "$QMAKE" ] || fail "$QMAKE not found.
       pass the SDK root as \$1 or export SDK=/path/to/<sdk-buildroot>"
[ -d "$SYS" ] || fail "sysroot $SYS not found (unexpected SDK layout?)"
[ -x "$CXX" ] || fail "$CXX not found"
"$CXX" --version >/dev/null 2>&1 || fail "$CXX cannot run (wrong architecture?)"

# 1) make the path baked into the Qt mkspec resolve
if [ -e "$LEGACY" ] || [ -L "$LEGACY" ]; then
    echo "== legacy path already present: $LEGACY"
else
    echo "== creating legacy symlink for the baked-in mkspec path"
    mkdir -p "$(dirname "$LEGACY")"
    if ln -s "$SDK" "$LEGACY" 2>/dev/null; then
        echo "   $LEGACY -> $SDK"
    else
        echo "   [warn] could not create $LEGACY; qmake may fail the compiler probe."
        echo "          Manual fix: mkdir -p $(dirname "$LEGACY") && ln -s $SDK $LEGACY"
    fi
fi

DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$DIR"
echo "== building park_ui in $DIR"
echo "== SDK    : $SDK"
echo "== sysroot: $SYS"

# 2) fresh Makefile, with the compiler overridden so the probe uses the real one
rm -f Makefile .qmake.stash
"$QMAKE" QMAKE_CC="$CC" QMAKE_CXX="$CXX" QMAKE_LINK="$CXX"

# 3) qmake still writes the legacy sysroot: rewrite it
sed -i "s|$LEGACY|$SDK|g; s|--sysroot=[^ ]*|--sysroot=$SYS|g" Makefile
grep -q "$SDK" Makefile || fail "sed did not patch the Makefile"
grep -q -- "--sysroot=$SYS" Makefile || echo "   [warn] --sysroot not found in Makefile"

# 4) force a recompile of everything we ship (new files, header changes)
touch src/*.cpp src/*.h
make -j4

echo
echo "== result"
ls -l bin/park_ui
file bin/park_ui 2>/dev/null || true
echo
echo "== step-7 build marker (must be >= 1; 0 means an old binary)"
strings bin/park_ui | grep -c 'python transport' || true
echo
echo "deploy:"
echo "  scp bin/park_ui root@<board>:/root/park_ui"
echo "  ssh root@<board> 'cp /root/park_ui /opt/park_ui/park_ui && chmod +x /opt/park_ui/park_ui && systemctl restart park-ui'"

#!/bin/sh
# build.sh - one-shot build and install script for aicoding
#
# Usage:
#   ./build.sh              # build only
#   sudo ./build.sh install # build and install to /usr/local/bin
#
# The installed `aicoding` command is a wrapper that sets LD_LIBRARY_PATH
# so libmydb.so / libvector_engine.so / libluajit-5.1.so can be found.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BINDIR="${BINDIR:-$PREFIX/bin}"

cd "$SCRIPT_DIR"

echo "[build.sh] Building aicoding in $SCRIPT_DIR ..."
make clean
make

echo "[build.sh] Build complete. Binary: $SCRIPT_DIR/aicoding"

if [ "$1" = "install" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        echo "[build.sh] install requires root. Run: sudo ./build.sh install"
        exit 1
    fi
    echo "[build.sh] Installing to $BINDIR ..."
    install -d "$BINDIR"
    install -m 755 aicoding "$BINDIR/aicoding.bin"
    install -m 755 aicoding.sh "$BINDIR/aicoding"
    echo "[build.sh] Installed. You can now run 'aicoding --project /path/to/project' from anywhere."
fi

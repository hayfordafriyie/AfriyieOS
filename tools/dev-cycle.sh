#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — build and boot, then report the result
#
# The edit/test loop in one command. This is the fastest way to answer "did that
# change work": it compiles everything, links, packages the image, boots it, and
# prints the markers.
#
# It is deliberately NOT tools/verify_all.sh. That one also runs the host test
# suite, the image verifier and the screenshot check, and takes several minutes.
# This is the one to run between edits.
#
# Usage:
#   ./tools/dev-cycle.sh              # default 180s boot timeout
#   ./tools/dev-cycle.sh 300          # slower machine, or a debug build

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

TIMEOUT="${1:-180}"
BUILD_DIR="${AF_BUILD_DIR:-build/x86_64}"
LOG="$BUILD_DIR/boot-serial.log"

echo "########## compile check ##########"
bash tools/compile_check.sh 2>&1 | tail -8 || exit 1

echo
echo "########## build ##########"
bash tools/build.sh 2>&1 | tail -6 || exit 1

echo
echo "########## boot ##########"
python3 tools/run_qemu.py --arch x86_64 \
    --image "$BUILD_DIR/afriyieos.img" \
    --test --timeout "$TIMEOUT" --serial-log "$LOG" 2>&1 | tail -6

echo
echo "########## markers ##########"
grep -a 'INFO  boot  : AF_' "$LOG" | tail -20 || true

echo
echo "########## anything fatal? ##########"
if grep -aq 'AF_PANIC\|EXCEPTION' "$LOG"; then
    sed -n '/AFRIYIEOS KERNEL PANIC/,$p' "$LOG"
    exit 1
fi
echo "  none"

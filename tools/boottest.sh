#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — boot the packaged image and print the marker summary
#
# Assumes the image already exists; it does not build one. Use dev-cycle.sh if
# you want the whole loop, or boottest-repeat.sh if you are chasing an
# intermittent failure.
#
# Usage:
#   ./tools/boottest.sh
#   ./tools/boottest.sh 300        # a longer timeout

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

TIMEOUT="${1:-120}"
BUILD_DIR="${AF_BUILD_DIR:-build/x86_64}"
IMAGE="${AF_IMAGE:-$BUILD_DIR/afriyieos.img}"
LOG="$BUILD_DIR/boot-serial.log"

if [ ! -f "$IMAGE" ]; then
    echo "no image at $IMAGE" >&2
    echo "build one with: bash tools/build.sh" >&2
    exit 1
fi

echo "=== boot test: $IMAGE ==="
python3 tools/run_qemu.py --arch x86_64 \
    --image "$IMAGE" \
    --test --timeout "$TIMEOUT" --serial-log "$LOG" 2>&1 \
    | grep -E 'PASS|FAIL|ok   AF|MISS' || true

echo
echo "=== final lines ==="
tail -n 12 "$LOG" 2>/dev/null || echo "(no log)"

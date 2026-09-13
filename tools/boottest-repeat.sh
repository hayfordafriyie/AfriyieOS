#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — run the boot test repeatedly
#
# The boot test has been intermittently flaky, and a bug that appears one run in
# five is a bug that will appear in CI at the worst moment. This runs it N times
# and reports which runs failed and what they were missing, so a flake can be
# characterised rather than dismissed.
#
# It deliberately does NOT retry a failed run. A retry turns "failed once in five"
# into "passed", which is exactly the information you needed.
#
# Usage:
#   ./tools/boottest-repeat.sh          # 5 runs
#   ./tools/boottest-repeat.sh 20       # 20 runs

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

RUNS="${1:-5}"
BUILD_DIR="${AF_BUILD_DIR:-build/x86_64}"
IMAGE="${AF_IMAGE:-$BUILD_DIR/afriyieos.img}"

pass=0
fail=0

for i in $(seq 1 "$RUNS"); do
    printf '\n===== run %d of %s =====\n' "$i" "$RUNS"

    # Stray QEMUs hold the disk image and the OVMF vars file, and a boot test
    # that fails because a previous one is still running looks exactly like a
    # kernel bug.
    pkill -9 qemu-system-x86_64 2>/dev/null
    sleep 0.5

    run_log="$BUILD_DIR/boot-run$i.log"

    if python3 tools/run_qemu.py --arch x86_64 \
            --image "$IMAGE" \
            --test --timeout 120 --serial-log "$run_log" 2>&1 | tail -n 4; then
        pass=$((pass + 1))
        echo "  RUN $i: PASS"
    else
        fail=$((fail + 1))
        echo "  RUN $i: FAIL"

        echo "  --- markers present ---"
        grep -a 'ok   AF\|MISS AF' "$run_log" 2>/dev/null || echo "    (no summary)"

        echo "  --- tail of the failing log ---"
        tail -n 15 "$run_log" 2>/dev/null
    fi
done

printf '\n===== %d passed, %d failed of %s =====\n' "$pass" "$fail" "$RUNS"
[ "$fail" -eq 0 ]

#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — native (host) tests for user-space C libraries
#
# Compiles a library and its tests with the HOST compiler and runs them. No
# cross-compiler, no kernel, no QEMU, no disk image — milliseconds instead of
# minutes.
#
# WHY THIS EXISTS
#
# tests/host is Python and tests the Python tooling. That leaves every C
# library that will run inside AfriyieOS untested until the whole system boots,
# which is the slowest and least informative way to find a parsing bug.
#
# libafpkg is the first, and the point of the harness is that the SECOND one
# costs nothing: add a directory under tests/native, list it below, done. The
# package manager, the file-system client and the personality runtimes all get
# host tests for free.
#
# Usage:
#   ./tools/native_test.sh            # every native suite
#   ./tools/native_test.sh afpkg      # one suite

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

CC="${AF_HOST_CC:-gcc}"
ONLY="${1:-}"

# Fixtures are generated, not downloaded — see tools/pkgsynth.py for why.
FIXTURES="build/fixtures"

if [ ! -d "$FIXTURES" ] || [ -z "$(ls -A "$FIXTURES" 2>/dev/null)" ]; then
    echo "=== generating package fixtures ==="
    python3 tools/pkgsynth.py --out "$FIXTURES" || exit 1
fi

mkdir -p build/native

# -----------------------------------------------------------------------------
# Warning flags: the same set the real build uses.
#
# A host test compiled with laxer warnings does not predict the target build, and
# a bookkeeping tool that disagrees with what it predicts is worse than none —
# which is exactly the bug that made link_check.sh reject a kernel that builds.
# -----------------------------------------------------------------------------
WARNINGS=(
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings
    -Wredundant-decls -Wno-unused-parameter -Werror
    -std=gnu11 -g -O1
    -Ikernel/include
    -Ilibs/libafpkg/include
    -Itests/native
)

failures=0
suites=0

run_suite() {
    local name="$1"; shift
    local sources=("$@")

    if [ -n "$ONLY" ] && [ "$ONLY" != "$name" ]; then
        return
    fi

    suites=$((suites + 1))

    echo
    echo "==============================================================="
    echo "  native suite: $name"
    echo "==============================================================="

    local binary="build/native/test_$name"

    if ! "$CC" "${WARNINGS[@]}" "${sources[@]}" -o "$binary" 2>&1; then
        echo "COMPILE FAILED: $name"
        failures=$((failures + 1))
        return
    fi

    if ! "$binary" "$FIXTURES"; then
        failures=$((failures + 1))
    fi
}

# -----------------------------------------------------------------------------
# The suites
# -----------------------------------------------------------------------------
run_suite afpkg \
    libs/libafpkg/afpkg.c \
    libs/libafpkg/inflate.c \
    tests/native/test_afpkg.c

# -----------------------------------------------------------------------------
echo
echo "==============================================================="
if [ "$failures" -eq 0 ]; then
    printf '  native tests: %d suite(s) passed\n' "$suites"
    echo "==============================================================="
    exit 0
fi
printf '  native tests: %d of %d suite(s) FAILED\n' "$failures" "$suites"
echo "==============================================================="
exit 1

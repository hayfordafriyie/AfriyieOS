#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — full verification
#
# Runs every check in order, from the ones that need nothing installed to the
# ones that boot the machine. This is what CI runs and what "is it green?" means.
#
# Usage:
#   ./tools/verify_all.sh              # everything, including a QEMU boot
#   ./tools/verify_all.sh --no-boot    # skip the emulator checks (fast)
#
# shellcheck disable=SC2317,SC2329
# The step functions below are invoked BY NAME through `step`, so shellcheck
# cannot see a call site and reports every one as unreachable/never invoked. The
# directive has to sit here rather than beside them: a `disable` applies to the
# next command only, and placing it before the block covered exactly one of the
# eight.
#
# TWO CODES FOR ONE FINDING, because the two shellcheck versions in play number
# it differently: 0.11.0 (a current local install) says SC2329, 0.9.0 (what
# `apt-get install shellcheck` gives on ubuntu-24.04, which is what CI uses) says
# SC2317. Disabling only SC2329 left CI red with the identical complaint.
#
# This is a real trade: SC2317 also covers genuinely dead code, and this disables
# it for the whole file. It is accepted because the script is a dispatcher whose
# entire purpose is indirect invocation — but if SC2317 ever fires here on
# something that is NOT one of the s_* functions, do not extend the directive;
# delete the dead code it found.
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

AF_CROSS_PREFIX="${AF_CROSS_PREFIX:-$HOME/opt/cross}"
export PATH="$AF_CROSS_PREFIX/bin:$PATH"

ARCH=x86_64
DO_BOOT=1
[ "${1:-}" = "--no-boot" ] && DO_BOOT=0

PASS=0
FAIL=0
SKIP=0
RESULTS=()

step() {
    local name="$1"; shift
    printf '\n\033[1;34m=== %s ===\033[0m\n' "$name"
    if "$@"; then
        PASS=$((PASS + 1))
        RESULTS+=("PASS  $name")
    else
        FAIL=$((FAIL + 1))
        RESULTS+=("FAIL  $name")
    fi
}

skip() {
    SKIP=$((SKIP + 1))
    RESULTS+=("SKIP  $1")
    printf '\n\033[1;33m=== %s: SKIPPED (%s) ===\033[0m\n' "$1" "$2"
}

# Individual steps, so `step` can run them by name.

s_pycompat()     { python3 tools/pycompat.py 2>&1 | tail -6; }
s_host_tests()   { python3 -m unittest discover -s tests/host 2>&1 | tail -4; }
s_native()       { bash tools/native_test.sh 2>&1 | tail -8; }
s_compile()      { bash tools/compile_check.sh; }
s_link()         { bash tools/link_check.sh; }
s_build()        { bash tools/build.sh; }
s_image()        { python3 tools/verify_image.py \
                       --image build/x86_64/afriyieos.img \
                       --expect "EFI/BOOT/BOOTX64.EFI:build/x86_64/boot/BOOTX64.EFI" \
                       --expect "INIT.ELF:build/x86_64/init.elf" \
                       | tail -3; }
s_boot()         { python3 tools/run_qemu.py --arch x86_64 \
                       --image build/x86_64/afriyieos.img \
                       --test --timeout 120 \
                       --serial-log build/x86_64/boot-serial.log | tail -8; }
s_screenshot()   { python3 tools/screenshot.py \
                       --image build/x86_64/afriyieos.img \
                       --output build/x86_64/splash.png \
                       --width 1280 --height 800 | tail -6; }

echo "==============================================================="
echo "  AfriyieOS full verification — target $ARCH"
echo "  toolchain: $AF_CROSS_PREFIX"
echo "==============================================================="

# -----------------------------------------------------------------------------
# Clean the environment first
#
# A stray QEMU from an interrupted run or a debugging session keeps the disk
# image and the shared OVMF variables file open, and the next boot test then
# fails in a way that looks like a kernel bug. That is not hypothetical: it is
# exactly what produced a "6 of 7 passed" result from a green tree, and a flaky
# gate is worse than a failing one because it teaches you to re-run instead of
# to look.
#
# The failure is not retried. Retrying would hide a real regression behind the
# same mechanism that hides a stray process, and the two are indistinguishable
# from the outside. Removing the hazard is the fix; the gate stays hard.
# -----------------------------------------------------------------------------
if pgrep -x qemu-system-x86_64 >/dev/null 2>&1; then
    printf '\033[1;33m==>\033[0m killing stray QEMU processes before starting\n'
    pkill -9 -x qemu-system-x86_64 2>/dev/null || true
    sleep 1
fi

rm -f /usr/share/afriyieos-OVMF_VARS.fd 2>/dev/null || true

# --- Tier 1: host only, no cross-compiler --------------------------------
# Tooling syntax against the CI interpreter FIRST, and not only because it is the
# fastest check. It is the one that CI runs before anything else, and when it
# fails there every later job is skipped — so a green local run that misses it is
# worth almost nothing. See tools/pycompat.py for the line of Python that hid the
# entire pipeline for four milestones.
step "Tooling syntax (CI interpreter)"    s_pycompat
step "Host tests (image toolchain)"       s_host_tests
# Native tests compile a user-space C library with the HOST compiler and
# exercise it against generated fixtures. They run before the cross build
# because they take a second and catch parsing bugs that would otherwise
# only surface once the whole system boots.
step "Native tests (user-space C)"        s_native
step "Compile check (all C + assembly)"   s_compile
step "Link check (script, symbols, layout)" s_link

# --- Tier 2/3: cross build and boot --------------------------------------
if [ -x "$AF_CROSS_PREFIX/bin/x86_64-elf-gcc" ]; then
    step "Cross build"                    s_build
    step "Image verification"             s_image

    if [ "$DO_BOOT" = 1 ]; then
        if command -v qemu-system-x86_64 >/dev/null 2>&1 && { [ -f /usr/share/OVMF/OVMF_CODE.fd ] || [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; } then
            step "QEMU boot test"         s_boot
            step "Screenshot verification" s_screenshot
        else
            skip "QEMU boot test" "qemu-system-x86 or OVMF not installed"
            skip "Screenshot verification" "qemu-system-x86 or OVMF not installed"
        fi
    else
        skip "QEMU boot test" "--no-boot"
        skip "Screenshot verification" "--no-boot"
    fi
else
    skip "Cross build" "x86_64-elf-gcc not found — run tools/build_toolchain.sh"
    skip "Image verification" "no build"
    skip "QEMU boot test" "no build"
    skip "Screenshot verification" "no build"
fi

echo
echo "==============================================================="
for line in "${RESULTS[@]}"; do
    case "${line%% *}" in
        PASS) printf '  \033[1;32m%s\033[0m\n' "$line" ;;
        FAIL) printf '  \033[1;31m%s\033[0m\n' "$line" ;;
        SKIP) printf '  \033[1;33m%s\033[0m\n' "$line" ;;
    esac
done
echo "---------------------------------------------------------------"
printf '  %d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
echo "==============================================================="

if [ "$FAIL" -ne 0 ]; then
    echo
    echo "VERIFICATION FAILED"
    exit 1
fi

echo
echo "ALL CHECKS PASSED"
exit 0

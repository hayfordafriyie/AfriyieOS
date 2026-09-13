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
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

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

s_host_tests()   { python3 -m unittest discover -s tests/host 2>&1 | tail -4; }
s_compile()      { bash tools/compile_check.sh; }
s_link()         { bash tools/link_check.sh; }
s_build()        { bash tools/build.sh; }
s_image()        { python3 tools/verify_image.py \
                       --image build/x86_64/afriyieos.img \
                       --expect "EFI/BOOT/BOOTX64.EFI:build/x86_64/boot/BOOTX64.EFI" \
                       | tail -3; }
s_boot()         { python3 tools/run_qemu.py --arch x86_64 \
                       --image build/x86_64/afriyieos.img \
                       --test --timeout 120 \
                       --serial-log build/x86_64/serial.log | tail -8; }
s_screenshot()   { python3 tools/screenshot.py \
                       --image build/x86_64/afriyieos.img \
                       --output build/x86_64/splash.png \
                       --width 1280 --height 800 | tail -6; }

echo "==============================================================="
echo "  AfriyieOS full verification — target $ARCH"
echo "  toolchain: $AF_CROSS_PREFIX"
echo "==============================================================="

# --- Tier 1: host only, no cross-compiler --------------------------------
step "Host tests (image toolchain)"       s_host_tests
step "Compile check (all C + assembly)"   s_compile
step "Link check (script, symbols, layout)" s_link

# --- Tier 2/3: cross build and boot --------------------------------------
if [ -x "$AF_CROSS_PREFIX/bin/x86_64-elf-gcc" ]; then
    step "Cross build"                    s_build
    step "Image verification"             s_image

    if [ "$DO_BOOT" = 1 ]; then
        if command -v qemu-system-x86_64 >/dev/null 2>&1 && [ -f /usr/share/OVMF/OVMF_CODE.fd -o -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
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

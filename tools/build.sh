#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — one-command build
#
# Configures and builds the kernel, the UEFI boot bridge and the bootable disk
# image for a target, then verifies the result.
#
# Usage:
#   ./tools/build.sh                 # x86_64, build + verify
#   ./tools/build.sh --run           # ... then boot in QEMU
#   ./tools/build.sh --test          # ... then run the headless boot test
#   ./tools/build.sh --clean         # wipe the build directory first
#   ./tools/build.sh --arch aarch64  # phone target (arrives in v1.1)
#
# Environment:
#   AF_CROSS_PREFIX   toolchain location (default: $HOME/opt/cross)
#   AF_JOBS           parallel jobs (default: nproc)
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ARCH="x86_64"
DO_RUN=0
DO_TEST=0
DO_CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)  ARCH="$2"; shift 2 ;;
        --run)   DO_RUN=1; shift ;;
        --test)  DO_TEST=1; shift ;;
        --clean) DO_CLEAN=1; shift ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

AF_CROSS_PREFIX="${AF_CROSS_PREFIX:-$HOME/opt/cross}"
JOBS="${AF_JOBS:-$(nproc 2>/dev/null || echo 4)}"

BUILD_DIR="build/$ARCH"
IMAGE="$BUILD_DIR/afriyieos.img"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

case "$ARCH" in
    x86_64)  TOOLCHAIN="cmake/toolchain-x86_64-elf.cmake" ;;
    aarch64) TOOLCHAIN="cmake/toolchain-aarch64-elf.cmake" ;;
    *)       die "unsupported arch '$ARCH' (x86_64 or aarch64)" ;;
esac

export PATH="$AF_CROSS_PREFIX/bin:$PATH"

# -----------------------------------------------------------------------------
# Preflight
# -----------------------------------------------------------------------------
info "AfriyieOS build — target $ARCH"

command -v cmake >/dev/null || die "cmake not found"
command -v nasm  >/dev/null || die "nasm not found (apt install nasm)"

if [ "$ARCH" = "x86_64" ]; then
    command -v x86_64-elf-gcc >/dev/null \
        || die "x86_64-elf-gcc not found under $AF_CROSS_PREFIX/bin
Run ./tools/build_toolchain.sh x86_64-elf first."
    command -v x86_64-w64-mingw32-gcc >/dev/null \
        || die "x86_64-w64-mingw32-gcc not found.
The UEFI boot bridge is a PE32+ executable and the x86_64-elf toolchain has no
PE emulation. Install it:  apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64"
    ok "x86_64-elf-gcc, nasm, mingw-w64 present"
else
    command -v aarch64-elf-gcc >/dev/null \
        || die "aarch64-elf-gcc not found under $AF_CROSS_PREFIX/bin"
fi

if [ "$DO_CLEAN" = 1 ] && [ -d "$BUILD_DIR" ]; then
    info "cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# -----------------------------------------------------------------------------
# Configure
# -----------------------------------------------------------------------------
info "configuring"
cmake -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
      -DAF_TARGET="$ARCH" \
      -DCROSS_PREFIX="$AF_CROSS_PREFIX" \
    || die "cmake configure failed"

# -----------------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------------
info "building ($JOBS jobs)"
cmake --build "$BUILD_DIR" -j "$JOBS" || die "build failed"

if [ "$ARCH" = "x86_64" ]; then
    [ -f "$BUILD_DIR/kernel.elf" ]     || die "kernel.elf was not produced"
    [ -f "$BUILD_DIR/boot/BOOTX64.EFI" ] || die "BOOTX64.EFI was not produced"

    ok "kernel.elf       $(( $(stat -c%s "$BUILD_DIR/kernel.elf") / 1024 )) KiB"
    ok "BOOTX64.EFI      $(stat -c%s "$BUILD_DIR/boot/BOOTX64.EFI") bytes"

    # Confirm the boot bridge really is an EFI application. A silent mistake
    # here produces an image firmware refuses to boot with no diagnostic.
    if command -v file >/dev/null; then
        if file "$BUILD_DIR/boot/BOOTX64.EFI" | grep -q "EFI (application)"; then
            ok "BOOTX64.EFI is a valid PE32+ EFI application"
        else
            die "$(file "$BUILD_DIR/boot/BOOTX64.EFI") — not an EFI application"
        fi
    fi

    # THE ENTRY POINT MUST BE AT THE KERNEL LINK BASE.
    # The boot bridge jumps to 0x100000 without reading the ELF header, so if
    # kernel_entry has drifted the image cannot boot and nothing reports why.
    if command -v x86_64-elf-nm >/dev/null; then
        ENTRY_ADDR=$(x86_64-elf-nm "$BUILD_DIR/kernel.elf" \
                     | awk '$3 == "kernel_entry" {print $1}')
        if [ "${ENTRY_ADDR}" != "0000000000100000" ]; then
            die "kernel_entry is at 0x${ENTRY_ADDR#0000000000}, expected 0x100000.
The boot bridge jumps to 0x100000 and does not read the ELF entry point.
Keep the entry stub in the .text.boot section placed first by the linker script."
        fi
        ok "kernel_entry at 0x100000 (matches the boot bridge jump target)"
    fi

    # Kernel size budget (blueprint section 14).
    if command -v x86_64-elf-size >/dev/null; then
        TEXT_SIZE=$(x86_64-elf-size -A "$BUILD_DIR/kernel.elf" | awk '/^\.text/ {print $2}')
        if [ "${TEXT_SIZE:-0}" -gt 65536 ]; then
            die "kernel .text is ${TEXT_SIZE} bytes, over the 64 KiB budget"
        fi
        ok "kernel .text    $TEXT_SIZE bytes (budget 65536)"
    fi
fi

# -----------------------------------------------------------------------------
# Package and verify the image
# -----------------------------------------------------------------------------
info "packaging the disk image"
python3 tools/mkimage.py --arch "$ARCH" --build-dir "$BUILD_DIR" \
        --output "$IMAGE" || die "image packaging failed"

info "verifying the image structure"
if [ "$ARCH" = "x86_64" ]; then
    python3 tools/verify_image.py --image "$IMAGE" \
        --expect "EFI/BOOT/BOOTX64.EFI:$BUILD_DIR/boot/BOOTX64.EFI" \
        || die "image verification failed"
else
    python3 tools/verify_image.py --image "$IMAGE" || die "image verification failed"
fi
ok "image verified"

# -----------------------------------------------------------------------------
# Optionally boot
# -----------------------------------------------------------------------------
if [ "$DO_TEST" = 1 ]; then
    info "booting in QEMU (headless marker test)"
    python3 tools/run_qemu.py --arch "$ARCH" --image "$IMAGE" \
        --test --timeout 90 --serial-log "$BUILD_DIR/serial.log" \
        || die "boot test failed"
fi

if [ "$DO_RUN" = 1 ]; then
    info "booting in QEMU"
    exec python3 tools/run_qemu.py --arch "$ARCH" --image "$IMAGE"
fi

echo
ok "build complete: $IMAGE"
echo
echo "  boot it:            python3 tools/run_qemu.py --arch $ARCH --image $IMAGE"
echo "  headless boot test: python3 tools/run_qemu.py --arch $ARCH --image $IMAGE --test"
echo "  debug with gdb:     python3 tools/run_qemu.py --arch $ARCH --image $IMAGE --debug"

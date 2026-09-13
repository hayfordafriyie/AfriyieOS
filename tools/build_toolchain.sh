#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — cross-compiler build script
#
# Builds bare-metal cross-compilers for both AfriyieOS targets:
#
#   x86_64-elf    -> the PC kernel and UEFI boot bridge
#   aarch64-elf   -> the phone kernel (used from milestone v1.1)
#
# Why build our own toolchain at all? A host gcc links against the host libc and
# host headers. Compiling a kernel with it produces references to functions that
# do not exist in our address space, and the resulting link failures are
# confusing rather than instructive. A *-elf target has no libc by design.
#
# The script is idempotent: each stage is skipped when its output already
# exists, so re-running after a partial failure resumes rather than restarts.
# A full run takes 20-40 minutes on a modern machine.
#
# Usage:
#   ./tools/build_toolchain.sh              # both targets
#   ./tools/build_toolchain.sh x86_64-elf   # one target
#
set -euo pipefail

BINUTILS_VERSION="2.42"
GCC_VERSION="14.2.0"

AF_PREFIX="${AF_CROSS_PREFIX:-$HOME/opt/cross}"
AF_SRC="${AF_CROSS_SRC:-$HOME/src/afriyieos-toolchain}"
AF_JOBS="${AF_JOBS:-$(nproc 2>/dev/null || echo 4)}"

TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=(x86_64-elf aarch64-elf)
fi

# -----------------------------------------------------------------------------
# Output helpers
# -----------------------------------------------------------------------------
info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()    { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m  !!\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# -----------------------------------------------------------------------------
# Prerequisite check
# -----------------------------------------------------------------------------
check_prerequisites() {
    info "Checking build prerequisites"

    local missing=()
    for tool in gcc g++ make bison flex wget tar; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done

    if [ ${#missing[@]} -gt 0 ]; then
        die "Missing tools: ${missing[*]}
Install them first:
  sudo apt install build-essential bison flex libgmp3-dev libmpc-dev \\
                   libmpfr-dev texinfo wget"
    fi

    # NASM is only needed for the x86_64 kernel assembly.
    if ! command -v nasm >/dev/null 2>&1; then
        warn "nasm not found — required to build the x86_64 kernel (apt install nasm)"
    fi

    ok "prerequisites present"
    mkdir -p "$AF_PREFIX" "$AF_SRC"
}

# -----------------------------------------------------------------------------
# Download
# -----------------------------------------------------------------------------
download_sources() {
    local tarball="$AF_SRC/binutils-$BINUTILS_VERSION.tar.xz"
    if [ ! -f "$tarball" ]; then
        info "Downloading binutils $BINUTILS_VERSION"
        wget -q --show-progress -O "$tarball" \
            "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
            || die "binutils download failed"
    fi
    if [ ! -d "$AF_SRC/binutils-$BINUTILS_VERSION" ]; then
        tar -xf "$tarball" -C "$AF_SRC"
    fi
    ok "binutils source ready"

    tarball="$AF_SRC/gcc-$GCC_VERSION.tar.xz"
    if [ ! -f "$tarball" ]; then
        info "Downloading gcc $GCC_VERSION"
        wget -q --show-progress -O "$tarball" \
            "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
            || die "gcc download failed"
    fi
    if [ ! -d "$AF_SRC/gcc-$GCC_VERSION" ]; then
        tar -xf "$tarball" -C "$AF_SRC"
    fi
    ok "gcc source ready"
}

# -----------------------------------------------------------------------------
# Binutils
# -----------------------------------------------------------------------------
build_binutils() {
    local target="$1"
    local build_dir="$AF_SRC/build-binutils-$target"

    if [ -x "$AF_PREFIX/bin/$target-ld" ]; then
        ok "binutils for $target already installed — skipping"
        return
    fi

    info "Building binutils $BINUTILS_VERSION for $target"
    mkdir -p "$build_dir"
    cd "$build_dir"

    "$AF_SRC/binutils-$BINUTILS_VERSION/configure" \
        --target="$target" \
        --prefix="$AF_PREFIX" \
        --with-sysroot \
        --disable-nls \
        --disable-werror \
        --enable-languages=c,c++ \
        || die "binutils configure failed"

    make -j"$AF_JOBS" || die "binutils build failed"
    make install     || die "binutils install failed"

    ok "binutils for $target installed into $AF_PREFIX"
}

# -----------------------------------------------------------------------------
# GCC
#
# Stage 1 only: all-gcc and all-target-libgcc. That is everything a freestanding
# kernel needs. There is deliberately no libstdc++ build for the *-elf targets:
# AfriyieOS user space uses its own mini-STL (libafpp) and links no standard
# library at all, so a cross libstdc++ would be dead weight.
# -----------------------------------------------------------------------------
build_gcc() {
    local target="$1"
    local build_dir="$AF_SRC/build-gcc-$target"

    if [ -x "$AF_PREFIX/bin/$target-gcc" ]; then
        ok "gcc for $target already installed — skipping"
        return
    fi

    info "Building gcc $GCC_VERSION (stage 1) for $target"
    mkdir -p "$build_dir"
    cd "$build_dir"

    "$AF_SRC/gcc-$GCC_VERSION/configure" \
        --target="$target" \
        --prefix="$AF_PREFIX" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --with-newlib \
        --disable-shared \
        --disable-threads \
        --disable-libssp \
        --disable-libgomp \
        --disable-libquadmath \
        --disable-libatomic \
        --disable-libstdcxx \
        --disable-decimal-float \
        || die "gcc configure failed"

    make -j"$AF_JOBS" all-gcc            || die "gcc all-gcc failed"
    make -j"$AF_JOBS" all-target-libgcc  || die "gcc all-target-libgcc failed"
    make install-gcc                     || die "gcc install-gcc failed"
    make install-target-libgcc           || die "gcc install-target-libgcc failed"

    ok "gcc for $target installed into $AF_PREFIX"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
    info "AfriyieOS cross-compiler build"
    info "  prefix : $AF_PREFIX"
    info "  sources: $AF_SRC"
    info "  jobs   : $AF_JOBS"
    info "  targets: ${TARGETS[*]}"

    check_prerequisites
    download_sources

    for target in "${TARGETS[@]}"; do
        case "$target" in
            x86_64-elf|aarch64-elf) ;;
            *) die "Unsupported target '$target' (expected x86_64-elf or aarch64-elf)" ;;
        esac
        build_binutils "$target"
        build_gcc "$target"
    done

    echo
    info "Cross-compilers ready. Add them to your PATH:"
    echo "    export PATH=\"$AF_PREFIX/bin:\$PATH\""
    echo
    for target in "${TARGETS[@]}"; do
        "$AF_PREFIX/bin/$target-gcc" --version | head -n 1
    done
    echo
    info "Next:"
    echo "    cmake -B build/x86_64 -G Ninja \\"
    echo "          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake \\"
    echo "          -DAF_TARGET=x86_64"
    echo "    cmake --build build/x86_64"
    echo "    cmake --build build/x86_64 --target run"
}

main

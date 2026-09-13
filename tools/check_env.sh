#!/bin/bash
# AfriyieOS — probe the build environment.
# Used by the developer and by CI to report what is and is not available,
# with the exact install command for anything missing.

set -u

have() { command -v "$1" >/dev/null 2>&1; }

ok=0
missing=()

check() {
    local name="$1" cmd="$2" why="$3"
    if have "$cmd"; then
        printf '  %-24s %s\n' "$name" "$(command -v "$cmd")"
        ok=$((ok + 1))
    else
        printf '  %-24s MISSING  (%s)\n' "$name" "$why"
        missing+=("$cmd")
    fi
}

echo "=== host ==="
echo "  uname:            $(uname -sr)"
echo "  cpus:             $(nproc 2>/dev/null || echo '?')"
echo "  python:           $(python3 --version 2>/dev/null || echo 'missing')"
echo

echo "=== build tools ==="
check "make"            make            "required"
check "gcc"             gcc             "host compiler, toolchain bootstrap"
check "bison"           bison           "toolchain bootstrap"
check "flex"            flex            "toolchain bootstrap"
check "nasm"            nasm            "x86_64 assembly"
check "cmake"           cmake           "required"
check "ninja"           ninja           "required"
echo

echo "=== cross toolchains ==="
check "x86_64-elf-gcc"  x86_64-elf-gcc  "run tools/build_toolchain.sh"
check "aarch64-elf-gcc" aarch64-elf-gcc "run tools/build_toolchain.sh (v1.1)"
echo

echo "=== UEFI boot bridge toolchain ==="
# The boot bridge is a PE32+ executable and the x86_64-elf toolchain has no PE
# emulation, so it is built with the packaged MinGW PE compiler instead.
check "x86_64-w64-mingw32-gcc" x86_64-w64-mingw32-gcc \
      "apt install gcc-mingw-w64-x86-64"
echo

echo "=== emulation ==="
check "qemu-system-x86_64" qemu-system-x86_64 "boot tests"
check "qemu-system-aarch64" qemu-system-aarch64 "v1.1 boot tests"

if [ -f /usr/share/OVMF/OVMF_CODE.fd ]; then
    echo "  OVMF_CODE.fd             /usr/share/OVMF/OVMF_CODE.fd"
elif [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
    echo "  OVMF_CODE_4M.fd          /usr/share/OVMF/OVMF_CODE_4M.fd"
else
    echo "  OVMF firmware            MISSING  (apt install ovmf)"
fi
echo

echo "=== summary ==="
if [ ${#missing[@]} -eq 0 ]; then
    echo "  all present ($ok checked)"
    exit 0
fi

echo "  ${#missing[@]} missing"
echo
echo "  Install on Ubuntu/Debian:"
echo "    sudo apt install build-essential bison flex libgmp3-dev libmpc-dev \\"
echo "                     libmpfr-dev texinfo nasm cmake ninja-build \\"
echo "                     qemu-system-x86 qemu-system-arm qemu-utils ovmf \\"
echo "                     gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64"
exit 1

#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — fast compile check
#
# Compiles every kernel and boot source file in freestanding mode using the HOST
# compiler, without producing a linked binary. This is a deliberately narrow
# tool with one job: tell you whether the code type-checks and assembles, in
# about two seconds, on any x86_64 Linux box, with no cross-compiler installed.
#
# Why this exists:
#
#   The cross-compiler takes 20-40 minutes to build. Waiting for it to discover a
#   missing semicolon or a wrong struct offset is a terrible feedback loop, and a
#   machine without it (a fresh clone, a CI container before the cache warms, a
#   contributor's laptop) can still catch almost every class of error:
#
#     * missing or misspelled includes
#     * type errors and implicit declarations
#     * every -Wall -Wextra warning, treated as an error
#     * AF_STATIC_ASSERT failures — the offset and size checks that guard the
#       UEFI and boot-info structures, which are the highest-risk code in the tree
#     * NASM syntax errors in the assembly
#
#   What it does NOT check: linking, the linker script, relocation ranges, and the
#   actual run. Those need the real toolchain; use tools/build.sh for that.
#
# The host x86_64 gcc has the same type sizes and the same ABI as x86_64-elf, so
# the static assertions and structure layouts are validated for real.
#
# Usage:
#   ./tools/compile_check.sh              # check everything
#   ./tools/compile_check.sh -v           # show each file as it is checked
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

CC="${AF_HOST_CC:-gcc}"
AS="${AF_HOST_AS:-nasm}"

# Kept deliberately identical to cmake/flags.cmake, minus the flags that are
# cross-toolchain specific. If the two drift, this tool stops predicting the real
# build and becomes worse than useless.
KERNEL_CFLAGS=(
    -ffreestanding
    -fno-stack-protector
    -fno-pic -fno-pie
    -fno-omit-frame-pointer
    -fno-builtin
    -mno-red-zone
    -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-avx
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings
    -Wredundant-decls -Wmissing-declarations -Wno-unused-parameter
    -Werror
    -std=gnu11
    -g -O0
    -DAF_DEBUG=1 -DAF_ASSERT_ENABLED=1
    -DAF_TARGET_X86_64=1 -DAF_TARGET_AARCH64=0
    -Ikernel/include
)

# The boot bridge is a UEFI application: Microsoft ABI, PE/COFF, no kernel model.
BOOT_CFLAGS=(
    -ffreestanding -fno-stack-protector -fshort-wchar
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx
    -maccumulate-outgoing-args
    -Wall -Wextra -Wno-unused-parameter
    -Werror
    -std=gnu11
    -g -O0
    -Ikernel/include
    -Iboot/uefi
)

c_ok=0;   c_fail=0
asm_ok=0; asm_fail=0
failures=()

# -----------------------------------------------------------------------------
# C
# -----------------------------------------------------------------------------
check_c() {
    local file="$1"; shift
    [ "$VERBOSE" = 1 ] && printf '  cc   %s\n' "$file"

    local err
    if err=$("$CC" "$@" -fsyntax-only "$file" 2>&1); then
        c_ok=$((c_ok + 1))
    else
        c_fail=$((c_fail + 1))
        failures+=("$file")
        printf '\n\033[1;31mFAIL\033[0m %s\n' "$file"
        printf '%s\n' "$err" | sed 's/^/     /'
    fi
}

echo "=== kernel C sources (host gcc, freestanding, -Werror) ==="
while IFS= read -r file; do
    check_c "$file" "${KERNEL_CFLAGS[@]}"
done < <(find kernel -name '*.c' | sort)

echo "=== boot bridge C sources (UEFI, ms_abi) ==="
while IFS= read -r file; do
    check_c "$file" "${BOOT_CFLAGS[@]}"
done < <(find boot -name '*.c' | sort)

# -----------------------------------------------------------------------------
# Assembly
# -----------------------------------------------------------------------------
check_asm() {
    local file="$1"
    [ "$VERBOSE" = 1 ] && printf '  as   %s\n' "$file"

    local out="/tmp/af-check-$(basename "$file").o"
    local err
    if err=$("$AS" -f elf64 -o "$out" "$file" 2>&1); then
        asm_ok=$((asm_ok + 1))
        rm -f "$out"
    else
        asm_fail=$((asm_fail + 1))
        failures+=("$file")
        printf '\n\033[1;31mFAIL\033[0m %s\n' "$file"
        printf '%s\n' "$err" | sed 's/^/     /'
    fi
}

if command -v "$AS" >/dev/null 2>&1; then
    echo "=== x86_64 assembly (nasm) ==="
    while IFS= read -r file; do
        check_asm "$file"
    done < <(find kernel/arch/x86_64 boot -name '*.asm' | sort)
else
    echo "=== assembly: SKIPPED (nasm not installed) ==="
fi

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
echo
echo "==============================================================="
printf '  C        : %3d ok, %3d failed\n' "$c_ok" "$c_fail"
if command -v "$AS" >/dev/null 2>&1; then
    printf '  assembly : %3d ok, %3d failed\n' "$asm_ok" "$asm_fail"
fi
echo "==============================================================="

if [ ${#failures[@]} -gt 0 ]; then
    echo
    echo "failed files:"
    for f in "${failures[@]}"; do
        echo "  $f"
    done
    echo
    echo "This check does NOT link. Once it passes, run the real cross build:"
    echo "  cmake -B build/x86_64 -G Ninja \\"
    echo "        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake -DAF_TARGET=x86_64"
    echo "  cmake --build build/x86_64"
    exit 1
fi

echo
echo "All sources compile and assemble cleanly."
exit 0

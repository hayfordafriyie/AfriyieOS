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
cd "$REPO_ROOT" || exit 1

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

# -Wa,--noexecstack marks the stack non-executable in every object; without
# it the linker assumes an executable stack, which is a real hardening loss.
#
# Held in a variable rather than written inline because shellcheck's SC2054
# reads the comma as an ARRAY ELEMENT SEPARATOR and warns about it, and it
# reports that at the line the array opens — so a disable directive on the
# flag's own line does not cover it. Splitting the flag would break the
# build; disabling the check file-wide would hide real findings elsewhere.
AF_NOEXECSTACK="-Wa,--noexecstack"

# -Wa,--noexecstack marks the stack non-executable in every object; without
# it the linker assumes an executable stack, which is a real hardening loss.
#
# Held in a variable rather than written inline because shellcheck's SC2054
# reads the comma as an ARRAY ELEMENT SEPARATOR and warns about it, and it
# reports that at the line the array opens — so a disable directive on the
# flag's own line does not cover it. Splitting the flag would break the
# build; disabling the check file-wide would hide real findings elsewhere.
AF_NOEXECSTACK="-Wa,--noexecstack"

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

# User space. Unprivileged, so it keeps the red zone and may use SSE — but it is
# linked at 4 GiB, above everything the small code model can address, so it needs
# -mcmodel=large. Leaving that out here would make this check unable to fail on
# the exact class of bug (a 32-bit absolute reference) that the real link catches,
# which is the whole point of the flag being in cmake/flags.cmake.
#
# -Wa,--noexecstack matters for the same reason as in the kernel: an object
# without a .note.GNU-stack section makes the linker assume an executable stack.
#
# The include paths are DISCOVERED, not listed. Listing them means every new
# library needs a matching edit here, and a checker that has to be remembered is
# a checker that will eventually be forgotten — which is exactly what happened
# when libafpkg was added and this file still knew only about libaf.
USER_INCLUDES=()
for dir in libs/*/include; do
    [ -d "$dir" ] && USER_INCLUDES+=("-I$dir")
done
USER_INCLUDES+=("-Ikernel/include")

USER_CFLAGS=(
    -ffreestanding
    -fno-stack-protector
    -fno-pic -fno-pie
    -fno-omit-frame-pointer
    -fno-builtin
    -mcmodel=large
    "$AF_NOEXECSTACK"
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings
    -Wredundant-decls -Wmissing-declarations -Wno-unused-parameter
    -Werror
    -std=gnu11
    -g -O0
    "${USER_INCLUDES[@]}"
)

c_ok=0;   c_fail=0
asm_ok=0; asm_fail=0
failures=()

# -----------------------------------------------------------------------------
# C
#
# Compiled to an object file with -c, NOT with -fsyntax-only.
#
# -fsyntax-only is faster but it stops after parsing and semantic analysis, so
# it never runs the whole-translation-unit passes that report -Wunused-function,
# -Wunused-variable and similar. A static function that no longer has a caller
# therefore passes this check and fails the real build — which is exactly what
# happened with pmm.c's mark_range_used. A check that predicts the build must
# actually do what the build does.
# -----------------------------------------------------------------------------
check_c() {
    local file="$1"; shift
    [ "$VERBOSE" = 1 ] && printf '  cc   %s\n' "$file"

    local obj
    obj="/tmp/af-check-$(echo "$file" | tr '/' '_').o"
    local err
    if err=$("$CC" "$@" -c "$file" -o "$obj" 2>&1); then
        c_ok=$((c_ok + 1))
        rm -f "$obj"
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

# User space was added to this check at v0.4, when it stopped being hypothetical.
# Before that, the only thing that compiled apps/ and libs/ was the real cross
# build — a several-minute round trip to discover a missing semicolon, which is
# precisely the feedback loop this script exists to shorten.
echo "=== user-space C sources (unprivileged, 4 GiB code model) ==="
while IFS= read -r file; do
    check_c "$file" "${USER_CFLAGS[@]}"
done < <(find libs apps -name '*.c' 2>/dev/null | sort)

# -----------------------------------------------------------------------------
# Assembly
# -----------------------------------------------------------------------------
check_asm() {
    local file="$1"
    [ "$VERBOSE" = 1 ] && printf '  as   %s\n' "$file"

    local out
    out="/tmp/af-check-$(basename "$file").o"
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

# GNU as sources. These are .S, not .asm: they go through the C preprocessor and
# are assembled by the C compiler, so they are checked with "$CC" and not nasm.
# The distinction is not cosmetic — it is exactly how crt0.S came to contain
# clang's `.section .note.GNU-stack noalloc noexec nowrite` word form, which GNU
# as rejects and LLVM accepts.
echo "=== user-space assembly (gas, via cc) ==="
while IFS= read -r file; do
    [ "$VERBOSE" = 1 ] && printf '  as   %s\n' "$file"
    obj="/tmp/af-check-$(echo "$file" | tr '/' '_').o"
    if err=$("$CC" -c "$file" -o "$obj" 2>&1); then
        asm_ok=$((asm_ok + 1)); rm -f "$obj"
    else
        asm_fail=$((asm_fail + 1)); failures+=("$file")
        printf '\n\033[1;31mFAIL\033[0m %s\n' "$file"
        printf '%s\n' "$err" | sed 's/^/     /'
    fi
done < <(find libs apps -name '*.S' 2>/dev/null | sort)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
echo
echo "==============================================================="
printf '  C        : %3d ok, %3d failed\n' "$c_ok" "$c_fail"
if command -v "$AS" >/dev/null 2>&1; then
    printf '  assembly : %3d ok, %3d failed   (nasm + gas)\n' "$asm_ok" "$asm_fail"
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

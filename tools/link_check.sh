#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — link check
#
# Compiles every kernel source and LINKS the result with the real linker script
# using the host toolchain, producing a genuine kernel ELF. Then it inspects the
# output: entry point, section addresses, and whether the linker symbols the
# assembly depends on actually resolved.
#
# This is the second half of tools/compile_check.sh. Between the two, almost
# every build error in the tree is caught in about three seconds, on any x86_64
# Linux box, with no cross-compiler installed:
#
#   compile_check.sh   type errors, warnings, static asserts, assembly syntax
#   link_check.sh      linker script syntax, symbol resolution, relocation
#                      ranges ("relocation truncated to fit"), section layout,
#                      and the entry point address
#
# What still needs the real cross-compiler: the UEFI application link (PE/COFF
# with a subsystem flag), and the boot itself.
#
# Usage:
#   ./tools/link_check.sh
#   ./tools/link_check.sh --keep     # leave the objects in build/linkcheck
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

OUT_DIR="build/linkcheck"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

CC="${AF_HOST_CC:-gcc}"
LD="${AF_HOST_LD:-ld}"
READELF="${AF_HOST_READELF:-readelf}"
SIZE="${AF_HOST_SIZE:-size}"
NM="${AF_HOST_NM:-nm}"
AS="${AF_HOST_AS:-nasm}"

KERNEL_CFLAGS=(
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie
    -fno-omit-frame-pointer -fno-builtin
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-avx
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    # These MUST match cmake/flags.cmake and tools/compile_check.sh, because the
    # whole job of this script is to predict the real build.
    #
    # -Wno-unused-parameter was missing here and present in both of the others,
    # so this check was STRICTER than the build it exists to predict: it rejected
    # a kernel that compiles. That is the same two-copies-of-one-truth problem as
    # the size budget, one level down — two checkers that disagree mean "the
    # compile check passed" is not a statement about anything.
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wwrite-strings
    -Wredundant-decls -Wmissing-declarations -Wno-unused-parameter
    -Werror
    -std=gnu11 -g -O0
    -DAF_DEBUG=1 -DAF_ASSERT_ENABLED=1
    -DAF_TARGET_X86_64=1 -DAF_TARGET_AARCH64=0
    -Ikernel/include
)

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
fail=0

echo "=== compiling kernel sources ==="
objs=()
while IFS= read -r src; do
    obj="$OUT_DIR/$(echo "$src" | tr '/' '_').o"
    if ! "$CC" "${KERNEL_CFLAGS[@]}" -c "$src" -o "$obj" 2>"$OUT_DIR/err.txt"; then
        echo "FAIL $src"
        sed 's/^/     /' "$OUT_DIR/err.txt"
        fail=1
    else
        objs+=("$obj")
    fi
done < <(find kernel -name '*.c' | sort)

echo "=== assembling x86_64 assembly ==="
while IFS= read -r src; do
    obj="$OUT_DIR/$(echo "$src" | tr '/' '_').o"
    if ! "$AS" -f elf64 -g -F dwarf -o "$obj" "$src" 2>"$OUT_DIR/err.txt"; then
        echo "FAIL $src"
        sed 's/^/     /' "$OUT_DIR/err.txt"
        fail=1
    else
        objs+=("$obj")
    fi
done < <(find kernel/arch/x86_64 -name '*.asm' | sort)

if [ "$fail" -ne 0 ]; then
    echo
    echo "compilation or assembly failed; not linking"
    exit 1
fi

echo "=== linking with kernel/linker/x86_64.lds ==="
if ! "$LD" -T kernel/linker/x86_64.lds \
           -nostdlib -static \
           --build-id=none --gc-sections \
           -Map="$OUT_DIR/kernel.map" \
           -o "$OUT_DIR/kernel.elf" "${objs[@]}" 2>"$OUT_DIR/err.txt"; then
    echo "FAIL link"
    sed 's/^/     /' "$OUT_DIR/err.txt"
    exit 1
fi
echo "  linked: $OUT_DIR/kernel.elf"

# A freestanding kernel needs __udivdi3/__umoddi3 for 64-bit division, supplied
# by libgcc. The real build links -lgcc; this check passes the host libgcc path
# explicitly so the check exercises the same symbols.
echo "=== resolving libgcc helper symbols ==="
UNDEF=$("$NM" -u "$OUT_DIR/kernel.elf" 2>/dev/null | awk '{print $2}' | sort -u)
if [ -n "$UNDEF" ]; then
    echo "  undefined symbols pulled from libgcc:"
    # Unquoted on purpose: UNDEF is a newline-separated list and each name
    # is a separate argument to printf's %s. Quoting it prints one line.
    # shellcheck disable=SC2086
    printf '    %s\n' $UNDEF
    LIBGCC=$("$CC" -print-libgcc-file-name)
    if [ -f "$LIBGCC" ]; then
        echo "  re-linking with $LIBGCC"
        if ! "$LD" -T kernel/linker/x86_64.lds -nostdlib -static \
                   --build-id=none --gc-sections \
                   -Map="$OUT_DIR/kernel.map" \
                   -o "$OUT_DIR/kernel.elf" "${objs[@]}" "$LIBGCC" \
                   2>"$OUT_DIR/err.txt"; then
            echo "FAIL re-link with libgcc"
            sed 's/^/     /' "$OUT_DIR/err.txt"
            exit 1
        fi
    fi
fi

# -----------------------------------------------------------------------------
# Inspect the result
# -----------------------------------------------------------------------------
echo
echo "=== kernel.elf ==="

ENTRY=$("$READELF" -h "$OUT_DIR/kernel.elf" | awk '/Entry point/ {print $4}')
echo "  entry point: $ENTRY"

# The entry point must land at the linker script's KERNEL_BASE (0x100000) plus
# whatever offset kernel_entry sits at in .text. A wildly different value means
# the linker script placed .text somewhere unexpected.
if [ "$ENTRY" != "0x100000" ] && [ "${ENTRY:0:4}" != "0x10" ]; then
    echo "  WARNING: entry point is not near 0x100000 — check the linker script"
    fail=1
fi

echo
echo "  sections:"
# With -W the fields are:  [ 1]  .text  PROGBITS  <addr>  <offset>  <size>  ...
# Note that "$1" is "[" and "$2" is "1]" — the bracket index is split into two
# fields by whitespace, so the name is $3, not $2.
"$READELF" -S -W "$OUT_DIR/kernel.elf" \
    | awk '$1 == "[" {
             name = $3; addr = $5; size = $7;
             if (name ~ /^\./ && size != "000000")
                 printf "    %-16s addr 0x%-12s size %s\n", name, addr, size
           }'

echo
echo "  symbols the assembly depends on:"
for sym in __bss_start __bss_end kernel_entry kmain kernel_stack_top; do
    ADDR=$("$NM" "$OUT_DIR/kernel.elf" | awk -v s="$sym" '$3 == s {print $1}')
    if [ -n "$ADDR" ]; then
        printf '    %-20s %s\n' "$sym" "0x$ADDR"
    else
        printf '    %-20s MISSING\n' "$sym"
        fail=1
    fi
done

echo
echo "  size:"
"$SIZE" -A "$OUT_DIR/kernel.elf" | awk '/^\.(text|rodata|data|bss)/ {printf "    %-10s %s\n", $1, $2}'

TEXT_SIZE=$("$SIZE" -A "$OUT_DIR/kernel.elf" | awk '/^\.text/ {print $2}')
echo
echo "  .text = ${TEXT_SIZE:-0} bytes (budget: 65536)"
if [ "${TEXT_SIZE:-0}" -gt 65536 ]; then
    echo "  WARNING: over the blueprint section 14 budget"
fi

# -----------------------------------------------------------------------------
# Linker script correctness checks
# -----------------------------------------------------------------------------
echo
echo "=== linker script checks ==="

# THE ENTRY POINT MUST BE AT THE KERNEL LINK BASE.
#
# The boot bridge copies the flat kernel binary to 0x100000 and jumps there
# without reading the ELF header. If kernel_entry is anywhere else, the CPU
# begins executing whatever code happens to sit at 0x100000 — with no stack and
# no arguments — and the machine hangs after ExitBootServices, where nothing can
# report the failure. This check has already caught that exact bug once.
ENTRY_ADDR=$("$NM" "$OUT_DIR/kernel.elf" | awk '$3 == "kernel_entry" {print $1}')
if [ "$ENTRY_ADDR" = "0000000000100000" ]; then
    echo "  kernel_entry is at 0x100000  OK (matches KERNEL_PHYS_BASE)"
else
    echo "  FAIL: kernel_entry is at 0x${ENTRY_ADDR#0000000000}, expected 0x100000"
    echo "        Put the entry stub in a dedicated .text.boot section that the"
    echo "        linker script places first; see kernel/arch/x86_64/entry.asm."
    fail=1
fi

# .bss must come after .data, and __bss_start/__bss_end must bracket it.
BSS_START=$("$NM" "$OUT_DIR/kernel.elf" | awk '$3 == "__bss_start" {print $1}')
BSS_SYMS=$("$NM" "$OUT_DIR/kernel.elf" | awk '$3 == "__bss_end" {print $1}')
if [ -n "$BSS_START" ] && [ -n "$BSS_SYMS" ]; then
    if [ $((16#$BSS_SYMS)) -gt $((16#$BSS_START)) ]; then
        echo "  __bss_start ($BSS_START) < __bss_end ($BSS_SYMS)  OK"
    else
        echo "  FAIL: __bss_end is not after __bss_start"
        fail=1
    fi
fi

# The EVMA (link address) of .text must be 0x100000 for v0.1.
TEXT_ADDR=$("$READELF" -S "$OUT_DIR/kernel.elf" \
            | awk '/\] \.text/ {print $5}')
if [ "$TEXT_ADDR" = "0000000000100000" ]; then
    echo "  .text loads at 0x100000  OK (matches KERNEL_PHYS_BASE in efi_main.c)"
else
    echo "  FAIL: .text is at 0x$TEXT_ADDR, expected 0x100000"
    echo "        The boot bridge copies the kernel to 0x100000; a mismatch here"
    echo "        means it would jump to an address the kernel was not linked for."
    fail=1
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "link check FAILED"
    exit 1
fi

echo "link check PASSED — the kernel links, the symbols resolve, and the layout"
echo "matches what the boot bridge expects."
echo

if [ "$KEEP" -eq 0 ]; then
    echo "(objects kept in $OUT_DIR; remove with: rm -rf $OUT_DIR)"
fi
exit 0

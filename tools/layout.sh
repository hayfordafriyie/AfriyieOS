#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — show where the kernel's key symbols landed
#
# Written to answer one question that cost real time: is a static object sitting
# inside the kernel stack region? That is what a .bss allocation overflowing into
# the stack looks like, and it presents as memory corruption thousands of
# instructions away from the cause.
#
# Usage:
#   ./tools/layout.sh
#   ./tools/layout.sh build/x86_64/kernel.elf

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

ELF="${1:-${AF_BUILD_DIR:-build/x86_64}/kernel.elf}"

NM="${AF_NM:-x86_64-elf-nm}"
if ! command -v "$NM" >/dev/null 2>&1; then
    NM="nm"
fi

READELF="${AF_READELF:-x86_64-elf-readelf}"
if ! command -v "$READELF" >/dev/null 2>&1; then
    READELF="readelf"
fi

if [ ! -f "$ELF" ]; then
    echo "no ELF at $ELF — build it first" >&2
    exit 1
fi

WANT='kernel_stack_bottom|kernel_stack_top|__bss_start|__bss_end|__kernel_end|s_bitmap|s_refcount|s_caches|s_free_frames|s_frame_count'

echo "=== key symbols ==="
"$NM" -n "$ELF" | grep -E " (${WANT})$" || true

echo
echo "=== .bss extent ==="
"$READELF" -S -W "$ELF" | awk '$1 == "[" && $3 == ".bss" {printf "  .bss  addr 0x%s  size %s bytes\n", $5, $7}'

echo
echo "=== interpretation ==="
SYMS="$(mktemp)"
trap 'rm -f "$SYMS"' EXIT
"$NM" -n "$ELF" > "$SYMS"

python3 - "$SYMS" <<'PY'
import sys

WANT = ("kernel_stack_bottom", "kernel_stack_top", "__bss_start", "__bss_end",
        "s_bitmap", "s_refcount", "s_caches", "s_free_frames", "s_frame_count")

values = {}
with open(sys.argv[1]) as handle:
    for line in handle:
        parts = line.split()
        if len(parts) == 3 and parts[2] in WANT:
            try:
                values[parts[2]] = int(parts[0], 16)
            except ValueError:
                pass

lo = values.get("kernel_stack_bottom")
hi = values.get("kernel_stack_top")

if lo is not None and hi is not None:
    print(f"  kernel stack occupies 0x{lo:x} .. 0x{hi:x}  ({(hi - lo) // 1024} KiB)")

bss_lo = values.get("__bss_start")
bss_hi = values.get("__bss_end")
if bss_lo is not None and bss_hi is not None:
    print(f"  .bss occupies         0x{bss_lo:x} .. 0x{bss_hi:x}"
          f"  ({(bss_hi - bss_lo) // 1024} KiB)")

if lo is not None and hi is not None:
    for name in ("s_bitmap", "s_refcount", "s_caches", "__bss_start"):
        if name not in values:
            continue
        where = values[name]
        if lo <= where < hi:
            print(f"  *** {name} at 0x{where:x} IS INSIDE THE STACK REGION ***")
        else:
            print(f"  {name} at 0x{where:x} is outside the stack region  OK")
PY

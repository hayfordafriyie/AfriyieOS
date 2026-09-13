#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — resolve an address in the kernel ELF to the nearest symbol
#
# For a panic dump: "rip : 0x00000000001068FE" means nothing on its own.
#
# Usage:
#   ./tools/resolve_addr.sh                 # a default address
#   ./tools/resolve_addr.sh 0x1068fe
#   ./tools/resolve_addr.sh 1068fe build/x86_64/kernel.elf

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

ADDR="${1:-1068fe}"
ELF="${2:-${AF_BUILD_DIR:-build/x86_64}/kernel.elf}"

NM="${AF_NM:-x86_64-elf-nm}"
if ! command -v "$NM" >/dev/null 2>&1; then
    NM="nm"
fi

if [ ! -f "$ELF" ]; then
    echo "no ELF at $ELF — build it first" >&2
    exit 1
fi

SYMS="$(mktemp)"
trap 'rm -f "$SYMS"' EXIT

"$NM" -n "$ELF" > "$SYMS"

# The address is parsed and compared numerically, so 0x1068fe, 1068fe and
# 0X1068FE all work — a panic dump and a symbol table rarely agree on a format.
python3 - "$ADDR" "$SYMS" <<'PY'
import sys

addr = int(sys.argv[1], 16)
best = None

with open(sys.argv[2]) as handle:
    for line in handle:
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            value = int(parts[0], 16)
        except ValueError:
            continue
        if value <= addr and (best is None or value > best[0]):
            best = (value, parts[1], parts[2])

if best is None:
    print(f"no symbol at or below 0x{addr:x}")
else:
    print(f"0x{addr:016x} is in {best[2]} + 0x{addr - best[0]:x}  "
          f"(symbol 0x{best[0]:x} {best[1]})")
PY

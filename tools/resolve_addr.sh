#!/bin/bash
# Resolve an address in the kernel ELF to the nearest preceding symbol.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS

export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

ADDR="${1:-1068fe}"
ELF="${2:-build/x86_64/kernel.elf}"

NM="${AF_NM:-x86_64-elf-nm}"
command -v "$NM" >/dev/null 2>&1 || NM=nm

"$NM" -n "$ELF" > /tmp/af-syms.txt

python3 - "$ADDR" <<'PY'
import sys, re

addr = int(sys.argv[1], 16)
best = None
with open('/tmp/af-syms.txt') as fh:
    for line in fh:
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
    print(f"0x{addr:016x} is in {best[2]} + 0x{addr - best[0]:x}  (symbol 0x{best[0]:x} {best[1]})")
PY

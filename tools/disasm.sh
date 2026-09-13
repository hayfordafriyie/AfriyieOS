#!/bin/bash
# Disassemble a region of the kernel around a symbol.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

SYM="${1:-pmm_alloc_frames}"
ELF="${2:-build/x86_64/kernel.elf}"

OBJDUMP="${AF_OBJDUMP:-x86_64-elf-objdump}"
command -v "$OBJDUMP" >/dev/null 2>&1 || OBJDUMP=objdump

"$OBJDUMP" -d --disassemble="$SYM" "$ELF" 2>/dev/null | head -120

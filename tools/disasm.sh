#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — disassemble a function in the kernel
#
# Usage:
#   ./tools/disasm.sh                       # a default symbol
#   ./tools/disasm.sh pmm_alloc_frames
#   ./tools/disasm.sh sched_tick build/x86_64/kernel.elf

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

SYM="${1:-pmm_alloc_frames}"
ELF="${2:-${AF_BUILD_DIR:-build/x86_64}/kernel.elf}"

OBJDUMP="${AF_OBJDUMP:-x86_64-elf-objdump}"
if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    OBJDUMP=objdump
fi

if [ ! -f "$ELF" ]; then
    echo "no ELF at $ELF — build it first" >&2
    exit 1
fi

"$OBJDUMP" -d --disassemble="$SYM" "$ELF" 2>/dev/null | head -120

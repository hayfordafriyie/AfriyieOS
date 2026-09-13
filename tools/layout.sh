#!/bin/bash
# Show the memory layout of the kernel's key symbols and .bss extent.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

NM="${AF_NM:-x86_64-elf-nm}"
command -v "$NM" >/dev/null 2>&1 || NM=nm

ELF="${1:-build/x86_64/kernel.elf}"

echo "=== key symbols ==="
"$NM" -n "$ELF" | grep -E " (kernel_stack_bottom|kernel_stack_top|__bss_start|__bss_end|__kernel_end|s_bitmap|s_refcount|s_caches|s_free_frames|s_frame_count)$"

echo
echo "=== .bss extent ==="
readelf -S -W "$ELF" | awk '$1 == "[" && $3 == ".bss" {printf "  .bss  addr 0x%s  size %s bytes (%d KiB)\n", $5, $7, strtonum("0x" $7)/1024}'

echo
echo "=== interpretation ==="
python3 - "$ELF" <<'PY'
import subprocess, sys, re
elf = sys.argv[1]
out = subprocess.run(['nm', '-n', elf], capture_output=True, text=True).stdout
out2 = subprocess.run(['/root/opt/cross/bin/x86_64-elf-nm', '-n', elf],
                      capture_output=True, text=True).stdout
out = out if out.strip() else out2

want = ('kernel_stack_bottom','kernel_stack_top','__bss_start','__bss_end','s_bitmap','s_refcount')
vals = {}
for line in out.splitlines():
    p = line.split()
    if len(p) == 3 and p[2] in want:
        vals[p[2]] = int(p[0], 16)

if 'kernel_stack_bottom' in vals and 'kernel_stack_top' in vals:
    lo, hi = vals['kernel_stack_bottom'], vals['kernel_stack_top']
    print(f"  kernel stack occupies 0x{lo:x} .. 0x{hi:x}  ({(hi-lo)//1024} KiB)")

if '__bss_start' in vals and '__bss_end' in vals:
    print(f"  .bss occupies         0x{vals['__bss_start']:x} .. 0x{vals['__bss_end']:x}"
          f"  ({(vals['__bss_end']-vals['__bss_start'])//1024} KiB)")

if 'kernel_stack_bottom' in vals and 'kernel_stack_top' in vals:
    lo, hi = vals['kernel_stack_bottom'], vals['kernel_stack_top']
    for name in ('s_bitmap', 's_refcount', '__bss_start'):
        if name in vals and lo <= vals[name] < hi:
            print(f"  *** {name} at 0x{vals[name]:x} IS INSIDE THE STACK REGION ***")
        elif name in vals:
            print(f"  {name} at 0x{vals[name]:x} is outside the stack region  OK")
PY

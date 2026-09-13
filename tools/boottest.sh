#!/bin/bash
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
echo "=== boot test markers ==="
python3 tools/run_qemu.py --arch x86_64 \
    --image build/x86_64/afriyieos.img \
    --test --timeout 120 \
    --serial-log build/x86_64/serial.log 2>&1 | grep -E "PASS|FAIL|ok   AF|MISS" | head -20
echo
echo "=== final lines ==="
tail -12 build/x86_64/serial.log

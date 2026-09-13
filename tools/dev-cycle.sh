#!/bin/bash
# Build and boot, then report the result. The edit/test loop in one command.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
export PATH="${AF_CROSS_PREFIX:-$HOME/opt/cross}/bin:$PATH"

TIMEOUT="${1:-180}"

echo "########## compile check ##########"
bash tools/compile_check.sh 2>&1 | tail -25 || exit 1

echo
echo "########## build ##########"
bash tools/build.sh 2>&1 | tail -20 || exit 1

echo
echo "########## boot ##########"
python3 tools/run_qemu.py --arch x86_64 \
    --image build/x86_64/afriyieos.img \
    --test --timeout "$TIMEOUT" \
    --serial-log build/x86_64/serial.log 2>&1 | tail -16

echo
echo "########## markers ##########"
grep -a "AF_.*READY\|AF_BOOT_OK\|AF_TEST_OK\|AF_SCHED_OK" build/x86_64/serial.log | tail -12

echo
echo "########## panic (if any) ##########"
grep -a -A3 "KERNEL PANIC" build/x86_64/serial.log | head -8

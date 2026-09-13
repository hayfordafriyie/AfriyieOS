#!/bin/bash
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
echo "=== user mode evidence ==="
grep -a "ring 3" build/x86_64/serial.log
grep -a "syscall" build/x86_64/serial.log
grep -a "user thread" build/x86_64/serial.log
echo
echo "=== last 20 lines ==="
tail -20 build/x86_64/serial.log

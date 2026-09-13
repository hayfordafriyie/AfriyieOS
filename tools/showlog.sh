#!/bin/bash
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
echo "=== tail of serial log ==="
tail -30 build/x86_64/serial.log
echo
echo "=== fs-related lines ==="
grep -a -e "gpt" -e "fat32" -e "test  :" build/x86_64/serial.log | tail -25

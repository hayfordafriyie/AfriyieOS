#!/bin/bash
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS
LOG=build/x86_64/serial.log

echo "=== last 40 log lines ==="
tail -40 "$LOG"

echo
echo "=== thread / sched lines ==="
grep -a "thread\|sched" "$LOG" | tail -15

#!/bin/bash
# Capture v0.4 evidence.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS

mkdir -p docs/releases/evidence

cp build/x86_64/serial.log docs/releases/evidence/v0.4.0-serial.log

{
    echo "AfriyieOS v0.4 user mode — captured from a real boot"
    echo "==============================================================="
    echo
    grep -a "user mode test" build/x86_64/serial.log
    grep -a "ring 3" build/x86_64/serial.log
    grep -a "user thread" build/x86_64/serial.log
    grep -a "system calls handled" build/x86_64/serial.log
    grep -a "AF_USER" build/x86_64/serial.log
} > docs/releases/evidence/v0.4.0-usermode.txt

echo "captured:"
cat docs/releases/evidence/v0.4.0-usermode.txt

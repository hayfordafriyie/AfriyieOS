#!/bin/bash
# Capture v0.2 release evidence from the boot log.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS

mkdir -p docs/releases/evidence

# Full log, with ANSI escapes stripped.
sed 's/\x1b\[[0-9;]*[A-Za-z]//g' build/x86_64/serial.log > docs/releases/evidence/v0.2.0-serial.log

# Just the scheduler portion, which is the acceptance evidence.
{
    echo "AfriyieOS v0.2 scheduler acceptance test — captured from a real boot"
    echo "======================================================================"
    echo
    grep -a "sched\|thread\|pmm\|heap\|test :" build/x86_64/serial.log \
        | grep -a -v "DEBUG test  :   ok   s\|DEBUG test  :   ok   c" \
        | tail -40
} > docs/releases/evidence/v0.2.0-scheduler.txt

echo "written:"
ls -la docs/releases/evidence/

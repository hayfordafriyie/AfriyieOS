#!/bin/bash
# Run the boot test repeatedly to characterise the intermittent failure.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS

RUNS="${1:-5}"
pass=0
fail=0

for i in $(seq 1 "$RUNS"); do
    printf '\n===== run %d of %s =====\n' "$i" "$RUNS"

    # Make sure no previous QEMU is still holding the disk or the vars file.
    pkill -9 qemu-system-x86_64 2>/dev/null
    sleep 0.5

    if python3 tools/run_qemu.py --arch x86_64 \
            --image build/x86_64/afriyieos.img \
            --test --timeout 120 \
            --serial-log "build/x86_64/serial-run$i.log" 2>&1 | tail -4; then
        pass=$((pass + 1))
        echo "  RUN $i: PASS"
    else
        fail=$((fail + 1))
        echo "  RUN $i: FAIL"

        echo "  --- markers present ---"
        for m in AF_GDT_READY AF_IDT_READY AF_PMM_READY AF_HEAP_READY \
                 AF_PAGING_READY AF_TEST_OK AF_VMM_OK AF_BOOT_OK \
                 AF_TIMER_READY AF_SCHED_READY AF_SCHED_OK \
                 AF_PCI_READY AF_BLOCK_OK AF_FS_OK; do
            if grep -aq "$m" "build/x86_64/serial-run$i.log" 2>/dev/null; then
                echo "    ok   $m"
            else
                echo "    MISS $m"
            fi
        done

        echo "  --- tail of the failing log ---"
        tail -15 "build/x86_64/serial-run$i.log" 2>/dev/null
    fi
done

printf '\n===== %d passed, %d failed of %s =====\n' "$pass" "$fail" "$RUNS"
[ "$fail" -eq 0 ]

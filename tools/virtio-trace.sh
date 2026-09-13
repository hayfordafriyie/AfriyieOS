#!/bin/bash
# Trace what the virtio device actually sees, to diagnose the request timeout.
set -uo pipefail
cd /mnt/c/code/acs/AfriyieOS

IMG=build/x86_64/afriyieos.img
CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
VARS=/tmp/af-trace-vars.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$VARS" 2>/dev/null || cp /usr/share/OVMF/OVMF_VARS.fd "$VARS"

TRACE=/tmp/af-virtio-trace.log
rm -f "$TRACE" /tmp/af-trace-serial.log

echo "=== available virtio trace events (sample) ==="
qemu-system-x86_64 -trace help 2>/dev/null | grep -i virtio | head -20

qemu-system-x86_64 \
  -machine q35 -m 2G -smp 1 \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -drive file="$IMG",format=raw,if=none,id=bootdisk \
  -device virtio-blk-pci,drive=bootdisk,disable-modern=on \
  -display none -vga std \
  -serial file:/tmp/af-trace-serial.log \
  -monitor none -no-reboot \
  -trace "enable=virtio_*,file=$TRACE" &
QPID=$!

for i in $(seq 1 60); do
    sleep 1
    if grep -aq "AF_BLOCK_OK\|ERR_TIMEOUT\|KERNEL PANIC" /tmp/af-trace-serial.log 2>/dev/null; then
        break
    fi
done
sleep 1
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null

echo
echo "=== kernel virtio log ==="
grep -a "virtio" /tmp/af-trace-serial.log | head -10

echo
echo "=== device-side trace ($(wc -l < "$TRACE" 2>/dev/null || echo 0) lines) ==="
head -40 "$TRACE" 2>/dev/null || echo "(no trace produced)"

echo
echo "=== unique trace event names seen ==="
sed 's/:.*//' "$TRACE" 2>/dev/null | sort | uniq -c | sort -rn | head -20

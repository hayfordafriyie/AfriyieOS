#!/bin/bash
# SPDX-License-Identifier: MIT
#
# AfriyieOS — show the boot serial log
#
# The kernel writes everything to COM1, and the boot test captures it to a file.
# This is the viewer: the tail, then the parts worth grepping for, then whichever
# section you asked for.
#
# Usage:
#   ./tools/showlog.sh                 # tail plus a summary
#   ./tools/showlog.sh user            # just the user-mode section
#   ./tools/showlog.sh fs              # just the file-system section
#   ./tools/showlog.sh panic           # only if something went wrong
#   ./tools/showlog.sh -n 200          # change how much tail
#   AF_LOG=path ./tools/showlog.sh     # a different log file

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

TAIL=40
SECTION=""

while [ $# -gt 0 ]; do
    case "$1" in
        -n) TAIL="${2:-40}"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) SECTION="$1"; shift ;;
    esac
done

LOG="${AF_LOG:-build/x86_64/boot-serial.log}"

if [ ! -f "$LOG" ]; then
    echo "no serial log at $LOG" >&2
    echo "run the boot test first:" >&2
    echo "  python3 tools/run_qemu.py --arch x86_64 \\" >&2
    echo "      --image build/x86_64/afriyieos.img --test" >&2
    exit 1
fi

show() {
    # -a because the log carries colour escapes and occasional binary noise.
    grep -a -i -e "$@" "$LOG"
}

case "$SECTION" in
    "")
        echo "=== last $TAIL lines of $LOG ==="
        tail -n "$TAIL" "$LOG"
        echo
        echo "=== milestones ==="
        show '^\[.*\] INFO  boot  : AF_' || true
        echo
        echo "=== anything fatal? ==="
        if show 'AF_PANIC|EXCEPTION|AF_TEST_FAIL' >/dev/null; then
            show 'AF_PANIC|EXCEPTION|fault|double free' || true
        else
            echo "  none"
        fi
        ;;
    user)
        echo "=== user mode ==="
        show 'ring 3|syscall|user thread|elf|init:' || true
        ;;
    fs)
        echo "=== partitions and file system ==="
        show 'gpt|fat32|/EFI|/HELLO|/INIT' || true
        ;;
    sched)
        echo "=== threads and scheduler ==="
        show 'thread|sched' || true
        ;;
    mem)
        echo "=== memory ==="
        show 'pmm|heap|vmm|paging|frame' || true
        ;;
    panic)
        if show 'AF_PANIC' >/dev/null; then
            # From the panic banner to the end: the dump is the useful part.
            sed -n '/AFRIYIEOS KERNEL PANIC/,$p' "$LOG"
        else
            echo "no panic in $LOG"
        fi
        ;;
    *)
        echo "unknown section '$SECTION'" >&2
        echo "one of: user, fs, sched, mem, panic" >&2
        exit 1
        ;;
esac

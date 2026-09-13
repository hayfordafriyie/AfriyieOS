#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AfriyieOS QEMU runner.

Two modes:

  interactive (default)
      Boots the image with a graphical window and the serial console attached to
      your terminal. This is the everyday development loop.

  test (--test)
      Boots headless, captures the serial output, waits for the kernel's
      AF_BOOT_OK marker, and exits with a status. This is what CI runs, and it
      is the reason the kernel prints machine-greppable milestones.

The test mode deliberately fails on any AF_PANIC: or AF_TEST_FAIL: marker, so a
kernel that boots but is internally broken is a failure, not a pass.

Usage:
    run_qemu.py --arch x86_64 --image build/x86_64/afriyieos.img
    run_qemu.py --arch x86_64 --image build/x86_64/afriyieos.img --test
    run_qemu.py --arch x86_64 --image build/x86_64/afriyieos.img --debug
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from typing import List, Optional

DEFAULT_TIMEOUT_SECONDS = 60

# Markers the kernel prints. Keeping the list here rather than in the kernel
# means a marker rename is a deliberate two-file change, not a silent CI break.
BOOT_MARKERS = [
    "AF_BOOT_OK",
]

# Ordered milestone markers. Missing any of these means a subsystem did not
# reach its ready state even if the boot completed.
#
# ORDER IS THE POINT. The runner stops the moment every marker is present, so a
# marker that is listed out of sequence — or missing from this list entirely —
# makes the test pass early and silently skip everything after it. That happened
# during v0.2: AF_BOOT_OK was the last expected marker, so the boot test declared
# success and killed QEMU before the scheduler had even started, and the
# multitasking work looked like it was passing when it had never run.
EXPECTED_MARKERS = [
    "AF_GDT_READY",
    "AF_IDT_READY",
    "AF_PMM_READY",
    "AF_HEAP_READY",
    "AF_PAGING_READY",
    "AF_TEST_OK",
    "AF_VMM_OK",
    "AF_BOOT_OK",
    "AF_TIMER_READY",
    "AF_SCHED_READY",
    "AF_SCHED_OK",
    "AF_PCI_READY",
    "AF_BLOCK_OK",
    "AF_FS_OK",
    "AF_USER_PREPARED",
    "AF_USER_OK",
    # v0.4, second half. AF_USER_OK comes from the built-in ring-3 stub, which
    # the kernel copies into a page it mapped itself. These two come from a
    # program read off the FAT32 volume as an ELF file — a different path
    # through the kernel, with the ELF parser, the per-segment page mapper and
    # the FAT32 reader all in it. Both stay in the list because they fail
    # independently: the stub passing says nothing about the loader.
    #
    # AF_EXEC_RAN is printed by the user program's own code, in ring 3, after
    # its checks have passed — so it is evidence that a program ran, not merely
    # that the kernel loaded one.
    "AF_EXEC_PREPARED",
    "AF_EXEC_RAN",
    # v0.6. Format identification for every foreign format AfriyieOS intends to
    # run — ELF, PE, Mach-O, DEX, APK, AAB, deb, rpm and the rest. It runs before
    # any personality exists, on synthetic headers, and it is in this list because
    # the detector is the one part of universal compatibility that can be verified
    # today. A regression here is a regression in the foundation every personality
    # will stand on.
    "AF_BINFMT_OK",
]

FATAL_MARKERS = [
    "AF_PANIC:",
    "AF_TEST_FAIL:",
]

DISPLAY_MODES = {
    "gtk": ["-display", "gtk"],
    "sdl": ["-display", "sdl"],
    "none": ["-display", "none"],
}


def log(message: str) -> None:
    print(f"[run_qemu] {message}")


def find_qemu(arch: str) -> str:
    binary = "qemu-system-x86_64" if arch == "x86_64" else "qemu-system-aarch64"
    path = shutil.which(binary)
    if path is None:
        sys.exit(
            f"{binary} not found on PATH.\n"
            f"  Ubuntu/Debian: sudo apt install qemu-system-x86 qemu-system-arm"
        )
    return path


def find_ovmf() -> Optional[tuple]:
    """Locate the OVMF firmware pair (code + writable vars).

    OVMF is packaged in several places across distributions, so try the known
    paths rather than assuming one.
    """
    code_candidates = [
        "/usr/share/OVMF/OVMF_CODE.fd",
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
        "/usr/share/ovmf/OVMF.fd",
        "/usr/share/edk2/x64/OVMF_CODE.fd",
        "/usr/share/edk2/ovmf/OVMF_CODE.fd",
        "/usr/share/qemu/OVMF_CODE.fd",
        "/usr/local/share/ovmf/OVMF_CODE.fd",
    ]
    vars_candidates = [
        "/usr/share/OVMF/OVMF_VARS.fd",
        "/usr/share/OVMF/OVMF_VARS_4M.fd",
        "/usr/share/edk2/x64/OVMF_VARS.fd",
        "/usr/share/edk2/ovmf/OVMF_VARS.fd",
        "/usr/share/qemu/OVMF_VARS.fd",
        "/usr/local/share/ovmf/OVMF_VARS.fd",
    ]

    code = next((p for p in code_candidates if os.path.isfile(p)), None)
    vars_path = next((p for p in vars_candidates if os.path.isfile(p)), None)

    if code is None:
        return None

    # The vars file must be a writable COPY: firmware writes its boot variables
    # into it, and modifying the packaged file requires root and upsets the
    # package manager.
    if vars_path is None:
        return None

    writable_vars = os.path.join(os.path.dirname(os.path.abspath(code)), "..",
                                 "afriyieos-OVMF_VARS.fd")
    writable_vars = os.path.abspath(writable_vars)
    try:
        shutil.copyfile(vars_path, writable_vars)
    except OSError:
        writable_vars = os.path.join(os.getcwd(), "afriyieos-OVMF_VARS.fd")
        shutil.copyfile(vars_path, writable_vars)

    return code, writable_vars


def build_command(args) -> List[str]:
    qemu = find_qemu(args.arch)
    cmd: List[str] = [qemu]

    if args.arch == "x86_64":
        cmd += ["-machine", "q35"]
        ovmf = find_ovmf()
        if ovmf is None:
            sys.exit(
                "OVMF firmware not found. AfriyieOS boots as a UEFI application "
                "and needs it.\n"
                "  Ubuntu/Debian: sudo apt install ovmf"
            )
        code, writable_vars = ovmf
        log(f"OVMF code: {code}")
        log(f"OVMF vars: {writable_vars}")
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={code}"]
        cmd += ["-drive", f"if=pflash,format=raw,file={writable_vars}"]
    else:
        cmd += ["-machine", "virt", "-cpu", "cortex-a72"]
        bios = os.environ.get("AF_ARM64_BIOS")
        if bios:
            cmd += ["-bios", bios]
        else:
            log("AF_ARM64_BIOS is not set; using the default firmware. The ARM64 "
                "port lands in v1.1 — see docs/AfriyieOS-Blueprint.md.")

    cmd += ["-m", args.memory, "-smp", str(args.smp)]

    # The disk is attached as virtio: the driver at v0.3 is written against
    # virtio, so using it from v0.1 keeps the device model consistent.
    cmd += ["-drive", f"file={args.image},format=raw,if=none,id=bootdisk"]

    # disable-modern=on forces the LEGACY virtio 0.9.5 interface, which presents
    # its registers as a flat block of I/O ports in BAR0.
    #
    # The driver could support the modern interface instead, but doing so means
    # parsing PCI vendor-specific capabilities to find where the registers are —
    # and the registers themselves are identical either way. Starting legacy and
    # adding modern discovery later is a smaller, testable step than doing both
    # at once. Making the flag explicit here means the driver's assumption is
    # stated rather than accidental: without it, QEMU presents a transitional
    # device and which interface wins depends on the driver's probe order.
    cmd += ["-device", "virtio-blk-pci,drive=bootdisk,disable-modern=on"]

    # Input devices, present from v0.1 so the v0.5 driver work has something to
    # bind to without changing the run configuration.
    cmd += ["-device", "virtio-keyboard-pci"]
    cmd += ["-device", "virtio-tablet-pci"]

    if args.no_reboot:
        cmd += ["-no-reboot"]

    if args.debug:
        cmd += ["-s", "-S"]
        log("GDB stub listening on tcp::1234 (CPU halted until you attach)")
        log("  gdb -ex 'target remote :1234' <kernel.elf>")

    return cmd


def run_interactive(args, cmd: List[str]) -> int:
    cmd += DISPLAY_MODES.get(args.display, DISPLAY_MODES["gtk"])
    cmd += ["-serial", "stdio"]

    log("booting " + args.image)
    log("  serial console: this terminal")
    log("  quit QEMU with Ctrl-A then X")
    print()

    try:
        return subprocess.call(cmd)
    except KeyboardInterrupt:
        return 130


def run_test(args, cmd: List[str]) -> int:
    serial_log = args.serial_log or os.path.join(
        os.path.dirname(os.path.abspath(args.image)), "serial.log"
    )

    cmd += ["-display", "none"]
    # A PIPE, not a file.
    #
    # `-serial file:PATH` looks like the obvious choice — the log survives a
    # timeout kill and can be attached to a CI failure — and it was used here
    # until it produced a failure that made no sense. QEMU writes that file
    # through a buffered FILE, so the last partial buffer stays in QEMU's memory
    # until it exits cleanly. Kill it, and those bytes are gone.
    #
    # The lost bytes are exactly the tail — and the tail is where the last
    # markers are. AF_EXEC_RAN, which the user program prints microseconds
    # before it exits and the machine goes quiet, sat inside that buffer: the
    # marker was present in every sense except on disk, and the boot test
    # reported MISS while printing the kernel's output that contained it. The
    # test was measuring QEMU's flush timing, not the kernel.
    #
    # A pipe has no such buffer. QEMU writes to the descriptor, the OS holds the
    # bytes, and the reader thread below drains them continuously — so nothing
    # is lost when the process is killed. The log file is still written, but as
    # a copy for humans rather than as the source of truth.
    cmd += ["-serial", "stdio"]
    cmd += ["-monitor", "none"]

    log(f"test mode: headless boot, {args.timeout}s timeout")
    log(f"serial log: {serial_log}")

    if os.path.exists(serial_log):
        os.remove(serial_log)

    handle = open(serial_log, "w", encoding="utf-8", errors="replace")

    process = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)

    # Read the pipe on its own thread so it never fills and blocks the guest.
    # A 64 KiB pipe buffer would stall QEMU mid-line if nobody drained it.
    chunks: List[bytes] = []
    reader_done = threading.Event()

    def drain() -> None:
        assert process.stdout is not None
        try:
            while True:
                block = process.stdout.read(4096)
                if not block:
                    break
                chunks.append(block)
                try:
                    handle.write(block.decode("utf-8", errors="replace"))
                    handle.flush()
                except (OSError, ValueError):
                    pass
        finally:
            reader_done.set()

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()

    def collected() -> str:
        return b"".join(chunks).decode("utf-8", errors="replace")

    deadline = time.time() + args.timeout
    output = ""
    success = False
    failure_reason = ""

    try:
        while time.time() < deadline:
            if process.poll() is not None:
                failure_reason = (f"QEMU exited early with status "
                                  f"{process.returncode}")
                break

            output = collected()

            if any(marker in output for marker in FATAL_MARKERS):
                failure_reason = "kernel reported a fatal marker"
                break

            if all(marker in output for marker in EXPECTED_MARKERS):
                success = True
                break

            time.sleep(0.25)
        else:
            failure_reason = f"timed out after {args.timeout}s"
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

        # Let the reader finish draining what the pipe still holds, so the
        # verdict and the log below are both judged on the complete output.
        reader_done.wait(timeout=5)
        reader.join(timeout=5)

        # Re-evaluate against everything that was captured. The timeout is still
        # the timeout; only the evidence used to judge it is now complete.
        output = collected()
        if not success:
            if any(marker in output for marker in FATAL_MARKERS):
                failure_reason = "kernel reported a fatal marker"
            elif all(marker in output for marker in EXPECTED_MARKERS):
                success = True
                failure_reason = ""

        try:
            handle.close()
        except (OSError, ValueError):
            pass

    print()
    print("=" * 68)
    print("SERIAL OUTPUT")
    print("=" * 68)
    print(output if output else "(no serial output captured)")
    print("=" * 68)

    if success:
        print()
        print("PASS: all boot markers present")
        for marker in EXPECTED_MARKERS:
            print(f"  ok   {marker}")
        return 0

    print()
    print(f"FAIL: {failure_reason}")
    for marker in EXPECTED_MARKERS:
        state = "ok  " if marker in output else "MISS"
        print(f"  {state} {marker}")
    for marker in FATAL_MARKERS:
        if marker in output:
            for line in output.splitlines():
                if marker in line:
                    print(f"  fatal: {line.strip()}")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot AfriyieOS in QEMU")
    parser.add_argument("--arch", required=True, choices=["x86_64", "aarch64"])
    parser.add_argument("--image", required=True, help="disk image to boot")
    parser.add_argument("--memory", default="2G", help="guest RAM (default: 2G)")
    parser.add_argument("--smp", type=int, default=1,
                        help="guest CPUs (default: 1 — SMP arrives in v1.2)")
    parser.add_argument("--display", default="gtk", choices=["gtk", "sdl", "none"])
    parser.add_argument("--debug", action="store_true",
                        help="halt at reset and expose a GDB stub on :1234")
    parser.add_argument("--test", action="store_true",
                        help="headless boot test: assert the kernel's markers")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_SECONDS,
                        help=f"test-mode timeout in seconds "
                             f"(default: {DEFAULT_TIMEOUT_SECONDS})")
    parser.add_argument("--serial-log", default=None,
                        help="where to capture serial output in test mode")
    parser.add_argument("--no-reboot", action="store_true", default=True,
                        help="do not reboot on triple fault (default: on)")

    args = parser.parse_args()

    if not os.path.isfile(args.image):
        sys.exit(
            f"image not found: {args.image}\n"
            f"Build it first:\n"
            f"    cmake --build <build-dir>\n"
            f"or:\n"
            f"    python3 tools/mkimage.py --arch {args.arch} "
            f"--build-dir <build-dir> --output {args.image}"
        )

    cmd = build_command(args)

    if args.test:
        return run_test(args, cmd)
    return run_interactive(args, cmd)


if __name__ == "__main__":
    sys.exit(main())

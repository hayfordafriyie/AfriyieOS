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
EXPECTED_MARKERS = [
    "AF_GDT_READY",
    "AF_IDT_READY",
    "AF_PMM_READY",
    "AF_HEAP_READY",
    "AF_TEST_OK",
    "AF_BOOT_OK",
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
        cmd += ["-device", "virtio-blk-pci,drive=bootdisk"]
    else:
        cmd += ["-machine", "virt", "-cpu", "cortex-a72"]
        bios = os.environ.get("AF_ARM64_BIOS")
        if bios:
            cmd += ["-bios", bios]
        else:
            log("AF_ARM64_BIOS is not set; using the default firmware. The ARM64 "
                "port lands in v1.1 — see docs/AfriyieOS-Blueprint.md.")
        cmd += ["-device", "virtio-blk-pci,drive=bootdisk"]

    cmd += ["-m", args.memory, "-smp", str(args.smp)]

    # The disk is attached as virtio: the driver at v0.3 is written against
    # virtio, so using it from v0.1 keeps the device model consistent.
    cmd += ["-drive", f"file={args.image},format=raw,if=none,id=bootdisk"]

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
    # file: rather than stdio so the log survives a timeout kill and can be
    # attached to a CI failure.
    cmd += ["-serial", f"file:{serial_log}"]
    cmd += ["-monitor", "none"]

    log(f"test mode: headless boot, {args.timeout}s timeout")
    log(f"serial log: {serial_log}")

    if os.path.exists(serial_log):
        os.remove(serial_log)

    process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)

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

            if os.path.exists(serial_log):
                # Reading a file that another process is still writing can fail
                # transiently. On a 9p/drvfs mount (WSL reading a Windows drive)
                # it raises ENODATA — "No data available" — for a log that simply
                # has not been flushed yet. That is not an error worth aborting a
                # boot test over, so transient failures are ignored and the next
                # poll retries.
                try:
                    with open(serial_log, "r", errors="replace") as handle:
                        output = handle.read()
                except OSError:
                    time.sleep(0.25)
                    continue

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

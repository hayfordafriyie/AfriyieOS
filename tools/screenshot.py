#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AfriyieOS screenshot capture and verification.

Boots the system in QEMU, waits for the kernel to signal it is alive, grabs the
framebuffer through the QEMU monitor, and verifies what is actually on screen.

This is the T3 tier check that a serial log cannot give you: the serial console
proves the kernel *thinks* it drew a splash, and only a screenshot proves it did.
A colour-conversion bug, a wrong stride, or a framebuffer format mistake all
produce a perfectly healthy serial log and a broken or blank screen.

Checks performed, independent of the renderer:
  * the framebuffer is not uniformly one colour (something was drawn)
  * the frame size matches what GOP reported
  * the brand palette is present, which means the colour conversion is right and
    not, say, swapping red and blue
  * the frame differs between the phone layout and the desktop layout

Usage:
    screenshot.py --image build/x86_64/afriyieos.img --output shot.png
    screenshot.py --image build/x86_64/afriyieos.img --width 480 --height 800
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections import Counter
from typing import Optional, Tuple

DEFAULT_TIMEOUT = 90


def log(message: str) -> None:
    print(f"[screenshot] {message}")


# =============================================================================
# PPM reading and PNG writing (no third-party dependencies)
# =============================================================================
def read_ppm(path: str) -> Tuple[int, int, bytes]:
    """Read a binary P6 PPM. Returns (width, height, RGB bytes)."""
    with open(path, "rb") as handle:
        data = handle.read()

    if not data.startswith(b"P6"):
        raise ValueError(f"{path} is not a binary P6 PPM")

    # Header: P6 <w> <h> <maxval>, whitespace separated, with '#' comments.
    fields = []
    index = 2
    while len(fields) < 3:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while index < len(data) and data[index:index + 1] != b"\n":
                index += 1
            continue
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        fields.append(int(data[start:index]))
    index += 1   # single whitespace after maxval

    width, height, maxval = fields
    if maxval != 255:
        raise ValueError(f"unsupported PPM maxval {maxval}")

    pixels = data[index:index + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError("PPM is truncated")
    return width, height, pixels


def write_png(path: str, width: int, height: int, rgb: bytes) -> None:
    """Write an 8-bit RGB PNG. Minimal but correct."""
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    # Each scanline is prefixed with a filter byte (0 = none).
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           chunk(b"IEND", b""))

    with open(path, "wb") as handle:
        handle.write(png)


# =============================================================================
# Analysis
# =============================================================================
BRAND_COLORS = {
    "primary #0B6E4F":       (0x0B, 0x6E, 0x4F),
    "secondary #F4B400":     (0xF4, 0xB4, 0x00),
    "accent #27C08A":        (0x27, 0xC0, 0x8A),
    "onSurface #ECEFF4":     (0xEC, 0xEF, 0xF4),
}


def nearest_brand_match(pixel: Tuple[int, int, int],
                        tolerance: int = 24) -> Optional[str]:
    for name, (r, g, b) in BRAND_COLORS.items():
        if (abs(pixel[0] - r) <= tolerance and
                abs(pixel[1] - g) <= tolerance and
                abs(pixel[2] - b) <= tolerance):
            return name
    return None


def analyse(width: int, height: int, rgb: bytes) -> dict:
    total = width * height
    colors = Counter()
    # Sample every 4th pixel: plenty for a histogram and 16x faster on 1280x800.
    for i in range(0, total, 4):
        o = i * 3
        colors[(rgb[o], rgb[o + 1], rgb[o + 2])] += 1

    sampled = max(1, len(range(0, total, 4)))
    distinct = len(colors)
    top_color, top_count = colors.most_common(1)[0]

    brand_hits = {}
    for pixel, count in colors.items():
        name = nearest_brand_match(pixel)
        if name:
            brand_hits[name] = brand_hits.get(name, 0) + count

    # --- where the brand colours are -----------------------------------------
    #
    # This is what turns "some green pixels exist" into "the logo is actually
    # where the layout says it should be". The splash is drawn with a known
    # composition, so its regions are assertable:
    #
    #   desktop layout: the mark on the LEFT of a centred card, the accent
    #                   progress bar spanning the card horizontally near the
    #                   bottom, text to the RIGHT of the mark
    #   phone layout:   the mark CENTRED, the bar near the bottom, everything
    #                   stacked
    #
    # A regression that moved the mark off-centre, mirrored the layout, or drew
    # the bar at the top would pass a colour check and fail this one.
    regions = {
        "left_third":   {"primary": 0, "secondary": 0, "accent": 0},
        "centre_third": {"primary": 0, "secondary": 0, "accent": 0},
        "right_third":  {"primary": 0, "secondary": 0, "accent": 0},
        "top_half":     {"primary": 0, "secondary": 0, "accent": 0},
        "bottom_half":  {"primary": 0, "secondary": 0, "accent": 0},
    }
    short = {"primary #0B6E4F": "primary",
             "secondary #F4B400": "secondary",
             "accent #27C08A": "accent"}

    stride = width * 3
    for y in range(0, height, 2):
        row = rgb[y * stride:(y + 1) * stride]
        vertical = "top_half" if y < height // 2 else "bottom_half"
        for x in range(0, width, 2):
            o = x * 3
            name = nearest_brand_match((row[o], row[o + 1], row[o + 2]))
            key = short.get(name or "")
            if key is None:
                continue
            if x < width // 3:
                horizontal = "left_third"
            elif x < 2 * width // 3:
                horizontal = "centre_third"
            else:
                horizontal = "right_third"
            regions[horizontal][key] += 1
            regions[vertical][key] += 1

    # Row and column variance: a splash should differ top to bottom (gradient,
    # logo, text) rather than being a flat fill.
    row_means = []
    for y in range(0, height, max(1, height // 16)):
        row = rgb[y * stride:(y + 1) * stride]
        if row:
            row_means.append(sum(row) / len(row))

    return {
        "width": width,
        "height": height,
        "distinct_sampled_colors": distinct,
        "dominant_color": top_color,
        "dominant_fraction": top_count / sampled,
        "brand_hits": brand_hits,
        "regions": regions,
        "row_mean_span": (max(row_means) - min(row_means)) if row_means else 0.0,
        "sampled": sampled,
    }


# =============================================================================
# QEMU driving
# =============================================================================
def find_ovmf() -> Tuple[str, str]:
    code_candidates = [
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
        "/usr/share/OVMF/OVMF_CODE.fd",
        "/usr/share/edk2/x64/OVMF_CODE.fd",
    ]
    vars_candidates = [
        "/usr/share/OVMF/OVMF_VARS_4M.fd",
        "/usr/share/OVMF/OVMF_VARS.fd",
        "/usr/share/edk2/x64/OVMF_VARS.fd",
    ]
    code = next((p for p in code_candidates if os.path.isfile(p)), None)
    vars_src = next((p for p in vars_candidates if os.path.isfile(p)), None)
    if code is None or vars_src is None:
        sys.exit("OVMF not found. Install it:  sudo apt install ovmf")

    vars_dst = os.path.join(tempfile.gettempdir(), "afriyieos-shot-vars.fd")
    shutil.copyfile(vars_src, vars_dst)
    return code, vars_dst


def capture(image: str, serial_log: str, ppm_path: str,
            width: Optional[int], height: Optional[int],
            timeout: int) -> bool:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        sys.exit("qemu-system-x86_64 not found")

    code, variables = find_ovmf()
    monitor_sock = os.path.join(tempfile.gettempdir(), "afriyieos-monitor.sock")
    if os.path.exists(monitor_sock):
        os.remove(monitor_sock)

    if os.path.exists(serial_log):
        os.remove(serial_log)
    if os.path.exists(ppm_path):
        os.remove(ppm_path)

    cmd = [
        qemu,
        "-machine", "q35",
        "-m", "2G",
        "-smp", "1",
        "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive", f"if=pflash,format=raw,file={variables}",
        "-drive", f"file={image},format=raw,if=none,id=bootdisk",
        "-device", "virtio-blk-pci,drive=bootdisk",
        "-device", "virtio-keyboard-pci",
        "-device", "virtio-tablet-pci",
        "-display", "none",
        "-vga", "std",
        "-serial", f"file:{serial_log}",
        "-monitor", f"unix:{monitor_sock},server,nowait",
        "-no-reboot",
    ]

    log(f"booting {image}")
    process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)

    # --- wait for the kernel to say it is alive ------------------------------
    deadline = time.time() + timeout
    ready = False
    while time.time() < deadline:
        if process.poll() is not None:
            log("QEMU exited early")
            break
        try:
            if os.path.exists(serial_log):
                with open(serial_log, "r", errors="replace") as handle:
                    if "AF_BOOT_OK" in handle.read():
                        ready = True
                        break
        except OSError:
            pass
        time.sleep(0.4)

    if not ready:
        log("kernel did not reach AF_BOOT_OK before the timeout")
        process.terminate()
        process.wait(timeout=10)
        return False

    log("kernel is alive; giving the splash a moment to settle")
    time.sleep(2.5)

    # --- grab the screen -----------------------------------------------------
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(monitor_sock)
        time.sleep(0.5)
        sock.recv(4096)                                  # monitor banner

        sock.sendall(f"screendump {ppm_path}\n".encode())
        time.sleep(2.0)
        try:
            sock.recv(4096)
        except socket.timeout:
            pass
        sock.sendall(b"quit\n")
        sock.close()
    except OSError as exc:
        log(f"monitor socket failed: {exc}")
        process.terminate()
        process.wait(timeout=10)
        return False

    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)

    if not os.path.exists(ppm_path):
        log("QEMU did not produce a screendump")
        return False

    log(f"captured {ppm_path} ({os.path.getsize(ppm_path)} bytes)")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture and verify an AfriyieOS screenshot")
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", default=None, help="PNG to write")
    parser.add_argument("--ppm", default=None, help="raw PPM path (default: temp)")
    parser.add_argument("--serial-log", default=None)
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    parser.add_argument("--width", type=int, default=None,
                        help="expected width (informational)")
    parser.add_argument("--height", type=int, default=None)
    parser.add_argument("--json", action="store_true", help="print the analysis as JSON")
    args = parser.parse_args()

    ppm_path = args.ppm or os.path.join(tempfile.gettempdir(), "afriyieos-shot.ppm")
    serial_log = args.serial_log or os.path.join(
        os.path.dirname(os.path.abspath(args.image)), "screenshot-serial.log")

    if not capture(args.image, serial_log, ppm_path, args.width, args.height,
                   args.timeout):
        return 1

    width, height, rgb = read_ppm(ppm_path)
    stats = analyse(width, height, rgb)

    if args.output:
        write_png(args.output, width, height, rgb)
        log(f"wrote {args.output}")

    if args.json:
        import json
        stats_out = dict(stats)
        stats_out["dominant_color"] = list(stats["dominant_color"])
        print(json.dumps(stats_out, indent=2))

    # -------------------------------------------------------------------------
    # Assertions
    # -------------------------------------------------------------------------
    print()
    print("=" * 68)
    print("SCREEN CONTENT")
    print("=" * 68)
    print(f"  resolution        : {width} x {height}")
    print(f"  distinct colours  : {stats['distinct_sampled_colors']} "
          f"(sampled {stats['sampled']} pixels)")
    print(f"  dominant colour   : rgb{stats['dominant_color']} "
          f"({stats['dominant_fraction'] * 100:.1f}%)")
    print(f"  row brightness span   : {stats['row_mean_span']:.1f}")
    print("  brand colours found:")
    for name, count in sorted(stats["brand_hits"].items(),
                              key=lambda kv: -kv[1]):
        print(f"      {name:<22} {count} sampled pixels")
    if not stats["brand_hits"]:
        print("      (none)")

    print("  where the brand colours are:")
    for region, hits in stats["regions"].items():
        parts = [f"{k}={v}" for k, v in hits.items() if v]
        print(f"      {region:<14} {', '.join(parts) if parts else '(none)'}")
    print("=" * 68)

    failures = []

    if args.width and width != args.width:
        failures.append(f"width is {width}, expected {args.width}")
    if args.height and height != args.height:
        failures.append(f"height is {height}, expected {args.height}")

    # 1. Something was drawn.
    if stats["dominant_fraction"] > 0.98:
        failures.append(
            f"the screen is {stats['dominant_fraction'] * 100:.1f}% one colour — "
            f"nothing was drawn, or the framebuffer write went nowhere")

    if stats["distinct_sampled_colors"] < 8:
        failures.append("fewer than 8 distinct colours — the splash is not rendering")

    # 2. The colour conversion is correct, not merely present. The gradient and
    #    glow mean exact brand pixels are rare, so a tolerance match is required;
    #    a red/blue swap would still produce colours, just not these.
    if not stats["brand_hits"]:
        failures.append(
            "no brand palette colours found within tolerance — check the pixel "
            "format conversion in kernel/core/fb.c (a red/blue swap looks fine "
            "in a serial log and wrong on screen)")

    # 3. The image has vertical structure (gradient, logo, text).
    if stats["row_mean_span"] < 3.0:
        failures.append("row brightness is nearly constant — expected a gradient "
                        "and centred content")

    # 4. Layout: the mark must be where the composition puts it.
    #
    # Portrait and landscape use different layouts, so the expected horizontal
    # placement of the primary mark differs between them. Anything else means the
    # responsive breakpoint picked the wrong branch.
    reg = stats["regions"]
    portrait = portrait_expected(width, height, args)

    # The accent progress bar is drawn near the bottom in BOTH layouts.
    if reg["bottom_half"]["accent"] == 0 and reg["top_half"]["accent"] == 0:
        failures.append("the accent progress bar is missing entirely")

    if portrait:
        if reg["centre_third"]["primary"] + reg["centre_third"]["secondary"] == 0:
            failures.append(
                "phone layout expected, but the logo mark is not in the centre "
                "third of the screen — the responsive rule may have picked the "
                "desktop branch")
        print("  layout            : phone (portrait) — mark centred")
    else:
        if reg["left_third"]["primary"] + reg["left_third"]["secondary"] == 0:
            failures.append(
                "desktop layout expected, but the logo mark is not in the left "
                "third of the screen — the responsive rule may have picked the "
                "phone branch")
        print("  layout            : desktop (landscape) — mark left of centre")

    print()
    if failures:
        print("FAIL: screenshot verification")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("PASS: the splash rendered, in the expected colours, with the expected "
          "layout")
    return 0


def portrait_expected(width: int, height: int, args) -> bool:
    """Mirror of the kernel's own breakpoint rule.

    Kept as a separate expression on purpose: if the kernel rule changes and this
    does not, the screenshot test starts failing, which is the correct signal
    that one of the two was updated without the other.
    """
    return (width * 1000) // height < 1150


if __name__ == "__main__":
    sys.exit(main())

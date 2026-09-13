# SPDX-License-Identifier: MIT
"""Host-side tests for the AfriyieOS image toolchain.

These are the T1 tier of the test strategy (docs/AfriyieOS-Blueprint.md
section 12.1): they run natively on the CI host with no cross-compiler, no QEMU
and no root, and they cover the parts of the build that are pure logic — which
is exactly where subtle corruption hides.

The important test here is `test_boot_file_round_trip`: it drives the FAT32
writer with a payload far larger than one cluster and then reads it back
through the GPT and the FAT chain, byte for byte. A FAT bug that only firmware
would otherwise notice fails here instead.

Run with:
    python3 -m pytest tests/host -v
or directly:
    python3 tests/host/test_mkimage.py
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

# Make tools/ importable regardless of where pytest is invoked from.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")
sys.path.insert(0, TOOLS_DIR)

import mkimage  # noqa: E402
import verify_image  # noqa: E402


def make_payload(size: int, seed: int = 12345) -> bytes:
    """Deterministic pseudo-random payload.

    Deterministic so a failure is reproducible, pseudo-random so that a writer
    bug that only shows on certain byte patterns (an off-by-one in a cluster
    copy, say) actually appears.
    """
    out = bytearray(size)
    state = seed
    for i in range(size):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] = (state >> 16) & 0xFF
    return bytes(out)


class TestFat32Geometry(unittest.TestCase):
    """The FAT size and cluster count must be mutually consistent."""

    def test_fat_is_large_enough_for_every_cluster(self):
        # 64 MiB with 512-byte clusters — the geometry mkimage uses by default.
        volume = mkimage.Fat32Volume(64 * 1024 * 1024 // 512)
        needed = ((volume.cluster_count + 2) * 4 + 511) // 512
        self.assertGreaterEqual(
            volume.fat_sectors, needed,
            "the FAT must be able to address every cluster it describes",
        )

    def test_cluster_count_is_valid_for_fat32(self):
        volume = mkimage.Fat32Volume(64 * 1024 * 1024 // 512)
        # Below 65 525 clusters a volume is FAT16 by definition, and firmware
        # will refuse to mount it as FAT32.
        self.assertGreaterEqual(volume.cluster_count, 65525)

    def test_fat32_rejects_a_volume_that_is_too_small(self):
        # 4 MiB cannot reach 65 525 clusters at any legal cluster size, so the
        # constructor must refuse rather than produce an invalid file system.
        with self.assertRaises(mkimage.Fat32Error):
            mkimage.Fat32Volume(4 * 1024 * 1024 // 512)

    def test_geometry_converges_for_a_range_of_sizes(self):
        for megabytes in (64, 128, 256):
            with self.subTest(megabytes=megabytes):
                volume = mkimage.Fat32Volume(megabytes * 1024 * 1024 // 512)
                needed = ((volume.cluster_count + 2) * 4 + 511) // 512
                self.assertGreaterEqual(volume.fat_sectors, needed)
                self.assertGreater(volume.data_sectors, 0)

    def test_32_mib_esp_is_rejected_because_fat32_cannot_fit(self):
        # 32 MiB at 512-byte clusters yields 65 536 sectors in total, which is
        # below the 65 525-cluster FAT32 floor once the FAT and reserved sectors
        # are subtracted. No cluster size rescues it, so the builder must refuse.
        with self.assertRaises(mkimage.Fat32Error) as context:
            mkimage.Fat32Volume(32 * 1024 * 1024 // 512)
        self.assertIn("65525", str(context.exception))


class TestFat32Writer(unittest.TestCase):
    """Round trips through the FAT32 writer without building a whole image."""

    def _volume(self) -> mkimage.Fat32Volume:
        return mkimage.Fat32Volume(64 * 1024 * 1024 // 512)

    def _read_back(self, volume: mkimage.Fat32Volume, path: str) -> bytes:
        reader = verify_image.Fat32Reader(volume.finalize())
        first, size, is_dir = reader.find(path)
        self.assertFalse(is_dir)
        return reader.read_file(first, size)

    def test_single_cluster_file(self):
        volume = self._volume()
        payload = b"AfriyieOS"
        volume.write_file("HELLO.TXT", payload)
        self.assertEqual(self._read_back(volume, "HELLO.TXT"), payload)

    def test_exact_multiple_of_cluster_size(self):
        # Boundary case: a file that is exactly N clusters is the easiest place
        # to allocate one cluster too few or too many.
        volume = self._volume()
        payload = make_payload(volume.bytes_per_cluster * 4)
        volume.write_file("EXACT.BIN", payload)
        self.assertEqual(self._read_back(volume, "EXACT.BIN"), payload)

    def test_one_byte_over_cluster_boundary(self):
        volume = self._volume()
        payload = make_payload(volume.bytes_per_cluster * 4 + 1)
        volume.write_file("OVER.BIN", payload)
        self.assertEqual(self._read_back(volume, "OVER.BIN"), payload)

    def test_one_byte_under_cluster_boundary(self):
        volume = self._volume()
        payload = make_payload(volume.bytes_per_cluster * 4 - 1)
        volume.write_file("UNDER.BIN", payload)
        self.assertEqual(self._read_back(volume, "UNDER.BIN"), payload)

    def test_large_multicluster_file(self):
        volume = self._volume()
        payload = make_payload(300 * 1024)
        volume.write_file("EFI/BOOT/BOOTX64.EFI", payload)
        self.assertEqual(self._read_back(volume, "EFI/BOOT/BOOTX64.EFI"), payload)

    def test_empty_file(self):
        volume = self._volume()
        volume.write_file("EMPTY.TXT", b"")
        self.assertEqual(self._read_back(volume, "EMPTY.TXT"), b"")

    def test_many_files_in_one_directory(self):
        # Each file costs a long-name entry plus a directory entry, so this
        # exercises directory growth and multi-entry appends in one cluster.
        volume = self._volume()
        expected = {}
        for index in range(200):
            name = f"file-{index:04d}.txt"
            payload = f"contents {index}".encode()
            expected[name] = payload
            volume.write_file(f"MANY/{name}", payload)

        for name, payload in expected.items():
            with self.subTest(name=name):
                self.assertEqual(self._read_back(volume, f"MANY/{name}"), payload)

    def test_nested_directories(self):
        volume = self._volume()
        volume.write_file("A/B/C/D/DEEP.TXT", b"deep")
        self.assertEqual(self._read_back(volume, "A/B/C/D/DEEP.TXT"), b"deep")

    def test_long_file_names_survive(self):
        volume = self._volume()
        name = "a-very-long-file-name-for-afriyieos.txt"
        volume.write_file(f"LONG/{name}", b"long name payload")
        self.assertEqual(self._read_back(volume, f"LONG/{name}").decode(),
                         "long name payload")

    def test_duplicate_file_is_rejected(self):
        volume = self._volume()
        volume.write_file("DUP.TXT", b"one")
        with self.assertRaises(mkimage.Fat32Error):
            volume.write_file("DUP.TXT", b"two")

    def test_short_name_checksum_matches_the_lfn_entries(self):
        # The checksum ties a long-name chain to its short entry. If it is wrong
        # the name is simply not found by anything that reads the volume.
        short = mkimage.Fat32Volume._short_name_for("BOOTX64.EFI", False)
        checksum = mkimage.Fat32Volume._short_name_checksum(short)
        lfn = mkimage.Fat32Volume._long_name_entries("BOOTX64.EFI", short, checksum)

        # Every 32-byte LFN entry must carry the same checksum.
        entry = None
        for offset in range(0, len(lfn), 32):
            entry = lfn[offset:offset + 32]
            self.assertEqual(entry[11], 0x0F, "LFN entries carry attribute 0x0F")
            self.assertEqual(entry[13], checksum)
        self.assertIsNotNone(entry)
        self.assertTrue(entry[0] & 0x40, "the last LFN entry sets the 0x40 bit")


class TestGpt(unittest.TestCase):
    """GPT structure and CRCs."""

    def test_header_crc_is_correct(self):
        entries = [
            mkimage.build_gpt_entry("ESP", mkimage.ESP_TYPE_GUID, 2048, 4095,
                                    b"\x11" * 16),
        ] + [b"\x00" * 128] * 127

        gpt = mkimage.build_gpt(135168, entries, b"\x22" * 16)
        header = gpt[0:512]

        self.assertEqual(header[0:8], b"EFI PART")

        stored = struct.unpack_from("<I", header, 16)[0]
        scratch = bytearray(header[0:92])
        struct.pack_into("<I", scratch, 16, 0)
        self.assertEqual(zlib.crc32(bytes(scratch)) & 0xFFFFFFFF, stored,
                         "header CRC32 must be computed with the CRC field zeroed")

    def test_entry_array_is_128_entries(self):
        entries = [mkimage.build_gpt_entry("ESP", mkimage.ESP_TYPE_GUID,
                                           2048, 4095, b"\x11" * 16)]
        entries += [b"\x00" * 128] * 127
        gpt = mkimage.build_gpt(135168, entries, b"\x22" * 16)
        header = gpt[0:512]

        self.assertEqual(struct.unpack_from("<I", header, 80)[0], 128)
        self.assertEqual(struct.unpack_from("<I", header, 84)[0], 128)

        stored = struct.unpack_from("<I", header, 88)[0]
        array = gpt[512:]
        self.assertEqual(zlib.crc32(array) & 0xFFFFFFFF, stored)

    def test_esp_type_guid_bytes(self):
        # C12A7328-F81F-11D2-BA4B-00A0C93EC93B in GPT's mixed-endian encoding.
        expected = bytes([
            0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
            0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
        ])
        self.assertEqual(mkimage.ESP_TYPE_GUID, expected)


class TestEndToEndImage(unittest.TestCase):
    """Build a real image and verify it with the independent reader."""

    def test_build_and_verify(self):
        payload = make_payload(300 * 1024)

        with tempfile.TemporaryDirectory() as tmp:
            boot_dir = os.path.join(tmp, "boot")
            os.makedirs(boot_dir)
            boot_path = os.path.join(boot_dir, "BOOTX64.EFI")
            with open(boot_path, "wb") as handle:
                handle.write(payload)

            image_path = os.path.join(tmp, "test.img")

            result = subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "mkimage.py"),
                 "--arch", "x86_64",
                 "--build-dir", tmp,
                 "--output", image_path],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0,
                             f"mkimage failed:\n{result.stdout}\n{result.stderr}")
            self.assertTrue(os.path.isfile(image_path))

            verify = subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "verify_image.py"),
                 "--image", image_path,
                 "--expect", f"EFI/BOOT/BOOTX64.EFI:{boot_path}"],
                capture_output=True, text=True,
            )
            self.assertEqual(verify.returncode, 0,
                             f"verify_image failed:\n{verify.stdout}\n{verify.stderr}")
            self.assertIn("all ", verify.stdout)
            self.assertIn("checks passed", verify.stdout)

    def test_accidental_corruption_is_detected(self):
        # The verifier must actually fail when something is wrong, otherwise the
        # previous test proves nothing. Flip one byte in the partition entry
        # array and confirm the CRC check catches it.
        payload = make_payload(64 * 1024)

        with tempfile.TemporaryDirectory() as tmp:
            boot_dir = os.path.join(tmp, "boot")
            os.makedirs(boot_dir)
            boot_path = os.path.join(boot_dir, "BOOTX64.EFI")
            with open(boot_path, "wb") as handle:
                handle.write(payload)

            image_path = os.path.join(tmp, "test.img")
            subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "mkimage.py"),
                 "--arch", "x86_64", "--build-dir", tmp, "--output", image_path],
                capture_output=True, check=True,
            )

            with open(image_path, "r+b") as handle:
                handle.seek(2 * 512 + 100)          # inside the GPT entry array
                original = handle.read(1)
                handle.seek(2 * 512 + 100)
                handle.write(bytes([original[0] ^ 0xFF]))

            verify = subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "verify_image.py"),
                 "--image", image_path],
                capture_output=True, text=True,
            )
            self.assertNotEqual(verify.returncode, 0,
                                "the verifier must reject a corrupted GPT")

    def test_aarch64_is_refused_with_a_clear_message(self):
        # The phone image is v1.1 work. Attempting it must fail with an
        # explanation, not a traceback.
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "mkimage.py"),
                 "--arch", "aarch64", "--build-dir", tmp,
                 "--output", os.path.join(tmp, "phone.img")],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("v1.1", result.stderr)


class TestBootInfoLayout(unittest.TestCase):
    """The kernel header must agree with what the boot bridge assumes.

    The C static asserts are the real check, but this catches the case where
    someone changes one definition and the offsets drift before the cross
    compiler ever runs — which is useful because the kernel cannot be built on
    every machine that edits it.
    """

    def test_boot_info_header_defines_expected_constants(self):
        header = os.path.join(REPO_ROOT, "kernel", "include", "afriyie",
                              "boot_info.h")
        with open(header, "r", encoding="utf-8") as handle:
            text = handle.read()

        self.assertIn("#define AF_BOOT_INFO_MAGIC", text)
        self.assertIn("#define AF_BOOT_INFO_VERSION", text)
        self.assertIn("#define AF_BOOT_INFO_BACKUP_ADDR", text)
        self.assertIn("#define AF_MAX_MEMORY_REGIONS", text)

        # The backup address must agree with what the boot bridge writes and the
        # fixed address the kernel falls back to.
        self.assertIn("0x0000000000007000ULL", text)

    def test_boot_bridge_uses_the_same_backup_address(self):
        # Both sides refer to the macro, so this is really checking that nobody
        # introduced a hardcoded second copy of the constant.
        source = os.path.join(REPO_ROOT, "boot", "uefi", "efi_main.c")
        with open(source, "r", encoding="utf-8") as handle:
            text = handle.read()

        self.assertIn("AF_BOOT_INFO_BACKUP_ADDR", text)
        self.assertNotIn("0x7000", text,
                         "the backup address must come from the shared header")

    def test_kernel_link_address_matches_the_boot_bridge(self):
        # If these drift apart, the boot bridge copies the kernel somewhere the
        # kernel was not linked for, and the first instruction fetch faults.
        linker = os.path.join(REPO_ROOT, "kernel", "linker", "x86_64.lds")
        with open(linker, "r", encoding="utf-8") as handle:
            linker_text = handle.read()

        source = os.path.join(REPO_ROOT, "boot", "uefi", "efi_main.c")
        with open(source, "r", encoding="utf-8") as handle:
            boot_text = handle.read()

        self.assertIn("KERNEL_BASE = 0x100000", linker_text)
        self.assertIn("#define KERNEL_PHYS_BASE  0x100000ULL", boot_text)


if __name__ == "__main__":
    unittest.main(verbosity=2)

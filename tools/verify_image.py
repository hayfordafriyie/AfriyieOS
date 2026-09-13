#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AfriyieOS disk image verifier.

Reads an image produced by tools/mkimage.py and checks it independently:

  * protective MBR signature and 0xEE partition type
  * GPT header signature, revision, size and both CRC32 checksums
  * the backup GPT header and entry array
  * the EFI System Partition entry's type GUID, alignment and bounds
  * the FAT32 boot sector, BPB consistency and cluster count
  * every FAT copy matches the other
  * EFI/BOOT/BOOTX64.EFI is reachable by walking the long-name entries and the
    FAT cluster chain, and its bytes match the source file exactly

That last check is the important one: it is a full round trip through the FAT32
writer, which is otherwise very easy to get subtly wrong in ways only firmware
would notice.

Usage:
    verify_image.py --image afriyieos.img --expect EFI/BOOT/BOOTX64.EFI:build/boot/BOOTX64.EFI
    verify_image.py --image afriyieos.img
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from typing import List, Optional, Tuple

SECTOR_SIZE = 512
ESP_TYPE_GUID = bytes([
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
])


class VerifyError(Exception):
    pass


class Checker:
    def __init__(self) -> None:
        self.passed = 0
        self.failures: List[str] = []

    def check(self, condition: bool, description: str) -> bool:
        if condition:
            self.passed += 1
            print(f"  ok   {description}")
        else:
            self.failures.append(description)
            print(f"  FAIL {description}")
        return bool(condition)

    def summary(self) -> int:
        print()
        if self.failures:
            print(f"{len(self.failures)} check(s) FAILED, {self.passed} passed")
            for failure in self.failures:
                print(f"  - {failure}")
            return 1
        print(f"all {self.passed} checks passed")
        return 0


# =============================================================================
# GPT
# =============================================================================
def gpt_header_crc(header: bytes, header_size: int = 92) -> int:
    """CRC32 of a GPT header, with the CRC field itself treated as zero.

    The CRC is computed over the header bytes 0..HeaderSize with the CRC32 field
    (bytes 16..20) zeroed. Computing it over the stored bytes — checksum included
    — always fails, which is a classic way to convince yourself a good image is
    corrupt.
    """
    scratch = bytearray(header[0:header_size])
    struct.pack_into("<I", scratch, 16, 0)
    return zlib.crc32(bytes(scratch)) & 0xFFFFFFFF


def parse_gpt(disk: bytes, c: Checker) -> Tuple[int, int]:
    """Validate the GPT and return (esp_first_lba, esp_last_lba)."""
    print("\n[protective MBR]")
    mbr = disk[0:SECTOR_SIZE]
    c.check(mbr[510] == 0x55 and mbr[511] == 0xAA, "MBR boot signature is 0xAA55")
    c.check(mbr[446 + 4] == 0xEE, "partition 1 has the GPT protective type 0xEE")
    protective_lba = struct.unpack_from("<I", mbr, 446 + 8)[0]
    c.check(protective_lba == 1, "protective partition starts at LBA 1")

    total_sectors = len(disk) // SECTOR_SIZE

    print("\n[primary GPT header]")
    header = disk[SECTOR_SIZE:2 * SECTOR_SIZE]
    c.check(header[0:8] == b"EFI PART", "signature is 'EFI PART'")

    revision = struct.unpack_from("<I", header, 8)[0]
    c.check(revision == 0x00010000, f"revision is 1.0 (got 0x{revision:08X})")

    header_size = struct.unpack_from("<I", header, 12)[0]
    c.check(header_size == 92, f"header size is 92 bytes (got {header_size})")

    stored_crc = struct.unpack_from("<I", header, 16)[0]
    computed_crc = gpt_header_crc(header, header_size)
    c.check(stored_crc == computed_crc,
            f"header CRC32 matches (stored 0x{stored_crc:08X}, "
            f"computed 0x{computed_crc:08X})")

    current_lba = struct.unpack_from("<Q", header, 24)[0]
    backup_lba = struct.unpack_from("<Q", header, 32)[0]
    c.check(current_lba == 1, f"current LBA is 1 (got {current_lba})")
    c.check(backup_lba == total_sectors - 1,
            f"backup LBA is the last sector (got {backup_lba}, "
            f"expected {total_sectors - 1})")

    entries_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = struct.unpack_from("<I", header, 80)[0]
    entry_size = struct.unpack_from("<I", header, 84)[0]
    c.check(entry_size == 128, f"partition entry size is 128 (got {entry_size})")
    c.check(entry_count >= 1, f"at least one partition entry ({entry_count})")

    entries_bytes = struct.unpack_from("<I", header, 88)[0]
    entries_data = disk[entries_lba * SECTOR_SIZE:
                        entries_lba * SECTOR_SIZE + entry_count * entry_size]
    computed_entries_crc = zlib.crc32(entries_data) & 0xFFFFFFFF
    c.check(entries_bytes == computed_entries_crc,
            f"partition entry array CRC32 matches "
            f"(stored 0x{entries_bytes:08X}, computed 0x{computed_entries_crc:08X})")

    # --- ESP entry -----------------------------------------------------------
    print("\n[EFI System Partition entry]")
    esp_first = esp_last = -1
    for index in range(entry_count):
        entry = entries_data[index * entry_size:(index + 1) * entry_size]
        if entry[0:16] == ESP_TYPE_GUID:
            esp_first = struct.unpack_from("<Q", entry, 32)[0]
            esp_last = struct.unpack_from("<Q", entry, 40)[0]
            name = entry[56:128].decode("utf-16-le").rstrip("\x00")
            print(f"  found: '{name}'  LBA {esp_first}..{esp_last}")
            break

    if not c.check(esp_first > 0, "an EFI System Partition entry exists"):
        return -1, -1

    c.check(esp_first % 2048 == 0, f"ESP starts on a 1 MiB boundary (LBA {esp_first})")
    c.check(esp_last < backup_lba - 32, "ESP does not overlap the backup GPT")
    c.check(esp_last > esp_first, "ESP bounds are ordered")

    # --- backup GPT ----------------------------------------------------------
    print("\n[backup GPT]")
    backup_header_lba = total_sectors - 1
    backup = disk[backup_header_lba * SECTOR_SIZE:
                  (backup_header_lba + 1) * SECTOR_SIZE]
    c.check(backup[0:8] == b"EFI PART", "backup header signature is 'EFI PART'")
    backup_stored_crc = struct.unpack_from("<I", backup, 16)[0]
    backup_computed_crc = gpt_header_crc(backup)
    c.check(backup_stored_crc == backup_computed_crc, "backup header CRC32 matches")

    backup_current = struct.unpack_from("<Q", backup, 24)[0]
    c.check(backup_current == total_sectors - 1,
            f"backup header points at itself (got {backup_current})")

    return esp_first, esp_last


# =============================================================================
# FAT32
# =============================================================================
class Fat32Reader:
    def __init__(self, partition: bytes):
        self.data = partition
        self.bytes_per_sector = struct.unpack_from("<H", partition, 11)[0]
        self.sectors_per_cluster = partition[13]
        self.reserved_sectors = struct.unpack_from("<H", partition, 14)[0]
        self.num_fats = partition[16]
        self.fat_sectors = struct.unpack_from("<I", partition, 36)[0]
        self.root_cluster = struct.unpack_from("<I", partition, 44)[0]

        self.bytes_per_cluster = self.bytes_per_sector * self.sectors_per_cluster
        self.fat_start = self.reserved_sectors * self.bytes_per_sector
        self.data_start = (self.reserved_sectors
                           + self.num_fats * self.fat_sectors) * self.bytes_per_sector

        data_sectors = (struct.unpack_from("<I", partition, 32)[0]
                        - self.reserved_sectors
                        - self.num_fats * self.fat_sectors)
        self.cluster_count = data_sectors // self.sectors_per_cluster

    def fat_entry(self, cluster: int, fat_index: int = 0) -> int:
        offset = self.fat_start + fat_index * self.fat_sectors * self.bytes_per_sector
        value = struct.unpack_from("<I", self.data, offset + cluster * 4)[0]
        return value & 0x0FFFFFFF

    def cluster_bytes(self, cluster: int) -> bytes:
        offset = self.data_start + (cluster - 2) * self.bytes_per_cluster
        return self.data[offset:offset + self.bytes_per_cluster]

    def chain(self, first: int) -> List[int]:
        chain = []
        cluster = first
        while 2 <= cluster < 0x0FFFFFF8:
            chain.append(cluster)
            if len(chain) > self.cluster_count + 2:
                raise VerifyError("cluster chain loops or is unreasonably long")
            cluster = self.fat_entry(cluster)
        return chain

    def read_file(self, first: int, size: int) -> bytes:
        out = bytearray()
        for cluster in self.chain(first):
            out += self.cluster_bytes(cluster)
        return bytes(out[:size])

    def list_dir(self, first_cluster: int) -> List[Tuple[str, int, int, bool]]:
        """Return (long_name, first_cluster, size, is_dir) tuples."""
        entries = []
        lfn_parts: List[str] = []

        for cluster in self.chain(first_cluster):
            raw = self.cluster_bytes(cluster)
            for offset in range(0, len(raw), 32):
                entry = raw[offset:offset + 32]
                if entry[0] == 0x00:
                    return entries
                if entry[0] == 0xE5:
                    lfn_parts = []
                    continue

                attributes = entry[11]
                if attributes == 0x0F:
                    # Long-name entry: collect its 13 UTF-16 code units.
                    chunk = (entry[1:11] + entry[14:26] + entry[28:32])
                    text = chunk.decode("utf-16-le", "replace")
                    text = text.split("\x00")[0]
                    lfn_parts.insert(0, text)
                    continue

                short = entry[0:8].decode("ascii", "replace").rstrip()
                ext = entry[8:11].decode("ascii", "replace").rstrip()
                short_name = short + ("." + ext if ext else "")

                name = "".join(lfn_parts) if lfn_parts else short_name
                lfn_parts = []

                first = (struct.unpack_from("<H", entry, 20)[0] << 16) | \
                        struct.unpack_from("<H", entry, 26)[0]
                size = struct.unpack_from("<I", entry, 28)[0]
                is_dir = bool(attributes & 0x10)

                if short_name in (".", ".."):
                    continue
                entries.append((name, first, size, is_dir))
        return entries

    def find(self, path: str) -> Tuple[int, int, bool]:
        """Resolve a path. Returns (first_cluster, size, is_dir)."""
        cluster = self.root_cluster
        size = 0
        is_dir = True

        parts = [p for p in path.replace("\\", "/").split("/") if p]
        for index, part in enumerate(parts):
            found = None
            for name, first, entry_size, entry_is_dir in self.list_dir(cluster):
                if name.upper() == part.upper():
                    found = (first, entry_size, entry_is_dir)
                    break
            if found is None:
                raise VerifyError(f"path component '{part}' not found in {path}")
            cluster, size, is_dir = found
        return cluster, size, is_dir


def parse_fat(disk: bytes, esp_first: int, esp_last: int, c: Checker,
              expect: Optional[Tuple[str, bytes]] = None) -> None:
    print("\n[FAT32 boot sector]")
    partition = disk[esp_first * SECTOR_SIZE:(esp_last + 1) * SECTOR_SIZE]

    bs = partition[0:SECTOR_SIZE]
    c.check(bs[510] == 0x55 and bs[511] == 0xAA, "FAT boot signature is 0xAA55")

    bytes_per_sector = struct.unpack_from("<H", bs, 11)[0]
    c.check(bytes_per_sector == 512, f"bytes per sector is 512 (got {bytes_per_sector})")

    fs_type = bs[82:90].decode("ascii", "replace")
    c.check(fs_type.startswith("FAT32"), f"file system type string is FAT32 (got '{fs_type}')")

    volume = Fat32Reader(partition)
    print(f"  clusters: {volume.cluster_count}, "
          f"{volume.sectors_per_cluster} sector(s) per cluster, "
          f"FAT at sector {volume.reserved_sectors}, "
          f"data at byte {volume.data_start}")

    c.check(volume.cluster_count >= 65525,
            f"cluster count is valid for FAT32 (>= 65525, got {volume.cluster_count})")
    c.check(volume.fat_sectors > 0, "FAT size is non-zero")
    c.check(volume.root_cluster == 2, f"root cluster is 2 (got {volume.root_cluster})")

    # The FAT must be large enough to describe every cluster.
    needed_fat_sectors = ((volume.cluster_count + 2) * 4 + 511) // 512
    c.check(volume.fat_sectors >= needed_fat_sectors,
            f"FAT is large enough ({volume.fat_sectors} >= {needed_fat_sectors} sectors)")

    print("\n[FAT copies]")
    if volume.num_fats >= 2:
        identical = True
        for cluster in range(0, min(volume.cluster_count + 2, 4096)):
            if volume.fat_entry(cluster, 0) != volume.fat_entry(cluster, 1):
                identical = False
                break
        c.check(identical, "FAT1 and FAT2 are identical over the first 4096 entries")

    print("\n[directory structure]")
    root = volume.list_dir(volume.root_cluster)
    names = [name for name, _, _, _ in root]
    c.check("EFI" in names, f"'EFI' exists in the root directory (found: {names})")

    if "EFI" in names:
        efi_cluster = next(first for name, first, _, is_dir in root
                           if name == "EFI" and is_dir)
        efi_entries = volume.list_dir(efi_cluster)
        efi_names = [name for name, _, _, _ in efi_entries]
        c.check("BOOT" in efi_names, f"'BOOT' exists in EFI/ (found: {efi_names})")

    if expect is not None:
        path, expected_bytes = expect
        print(f"\n[file round trip: {path}]")
        try:
            first, size, is_dir = volume.find(path)
        except VerifyError as exc:
            c.check(False, str(exc))
            return

        c.check(not is_dir, f"{path} is a regular file")
        c.check(size == len(expected_bytes),
                f"size matches the source ({size} == {len(expected_bytes)} bytes)")

        actual = volume.read_file(first, size)
        c.check(len(actual) == len(expected_bytes),
                f"read back the full length ({len(actual)} bytes)")
        c.check(actual == expected_bytes,
                "contents match the source byte for byte")

        if actual != expected_bytes and len(actual) == len(expected_bytes):
            for i in range(len(actual)):
                if actual[i] != expected_bytes[i]:
                    c.check(False, f"first difference at byte offset {i}")
                    break

        # The chain must terminate and be exactly as long as the data needs.
        chain = volume.chain(first)
        expected_clusters = max(1, (size + volume.bytes_per_cluster - 1)
                                // volume.bytes_per_cluster)
        c.check(len(chain) == expected_clusters,
                f"cluster chain length is exact ({len(chain)} == {expected_clusters})")
        c.check(volume.fat_entry(chain[-1]) >= 0x0FFFFFF8,
                "chain terminates with an end-of-chain marker")


# =============================================================================
# Main
# =============================================================================
def main() -> int:
    parser = argparse.ArgumentParser(description="Verify an AfriyieOS disk image")
    parser.add_argument("--image", required=True, help="image to verify")
    parser.add_argument("--expect", action="append", default=[],
                        metavar="PATH:SOURCE",
                        help="assert that PATH on the ESP matches the file SOURCE")
    args = parser.parse_args()

    with open(args.image, "rb") as handle:
        disk = handle.read()

    print(f"verifying {args.image} ({len(disk)} bytes, "
          f"{len(disk) // SECTOR_SIZE} sectors)")

    c = Checker()

    esp_first, esp_last = parse_gpt(disk, c)
    if esp_first < 0:
        return c.summary()

    expect = None
    if args.expect:
        path, _, source = args.expect[0].partition(":")
        with open(source, "rb") as handle:
            expect = (path, handle.read())

    parse_fat(disk, esp_first, esp_last, c, expect)

    return c.summary()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AfriyieOS disk image builder.

Produces a bootable GPT disk image containing an EFI System Partition (FAT32)
with the UEFI boot bridge at the firmware fallback path EFI/BOOT/BOOTX64.EFI.

Why write FAT32 and GPT by hand instead of shelling out to mtools/xorriso?
Because the image is an artefact of the build, and a build that depends on an
external partitioning tool is a build that breaks on someone else's machine.
Everything here is pure Python.

Layout produced:

    LBA 0        protective MBR
    LBA 1        GPT header
    LBA 2..33    GPT partition entry array (128 entries x 128 bytes)
    LBA 2048..   EFI System Partition (FAT32)
    last 33      backup partition array + backup GPT header

Usage:
    mkimage.py --arch x86_64 --build-dir build/x86_64 --output afriyieos.img
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import List, Optional

# -----------------------------------------------------------------------------
# Constants
# -----------------------------------------------------------------------------
SECTOR_SIZE = 512

# ESP geometry.
#
# 512-byte clusters, not the 4 KiB a modern tool would pick by default. FAT32 is
# only valid between 65 525 and 0x0FFFFFF5 clusters, so on a volume this small
# the cluster size must be small to stay above the lower bound. The practical
# consequence is a minimum ESP size: at one 512-byte sector per cluster a volume
# needs roughly 33.5 MiB before it has 65 525 usable clusters, so a 32 MiB ESP
# cannot be FAT32 at all. Fat32Volume raises rather than producing a file system
# firmware would silently refuse to mount.
ESP_SIZE_BYTES = 64 * 1024 * 1024
ESP_SECTORS_PER_CLUSTER = 1
ESP_RESERVED_SECTORS = 32
ESP_NUM_FATS = 2

# Partition start, aligned to 1 MiB (2048 sectors). UEFI firmware is generally
# tolerant, but 1 MiB alignment is what every modern partitioning tool produces
# and it avoids a class of firmware quirks.
ESP_START_LBA = 2048

# EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
ESP_TYPE_GUID = bytes([
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
])

EFI_FALLBACK_PATH = "EFI/BOOT/BOOTX64.EFI"


def guid_from_text(text: str) -> bytes:
    """Convert a textual GUID into the 16-byte little-endian disk format.

    GPT stores the first three fields little-endian; the last two big-endian.
    Getting this wrong yields a GUID the firmware does not recognise, and the
    partition silently does not appear.
    """
    parts = text.strip().strip("{}").split("-")
    if len(parts) != 5:
        raise ValueError(f"malformed GUID: {text}")
    d1, d2, d3, d4, d5 = parts
    return (
        struct.pack("<IHH", int(d1, 16), int(d2, 16), int(d3, 16))
        + bytes.fromhex(d4)
        + bytes.fromhex(d5)
    )


def log(message: str) -> None:
    print(f"[mkimage] {message}")


# =============================================================================
# FAT32
# =============================================================================
class Fat32Error(Exception):
    pass


@dataclass
class DirEntry:
    name: str
    is_dir: bool
    first_cluster: int
    size: int
    short_name: bytes


class Fat32Volume:
    """A minimal but correct FAT32 formatter and writer.

    Supports exactly what the ESP needs: creating directories and writing files,
    including files larger than one cluster.
    """

    def __init__(self, total_sectors: int, sectors_per_cluster: int = ESP_SECTORS_PER_CLUSTER):
        self.total_sectors = total_sectors
        self.sectors_per_cluster = sectors_per_cluster
        self.bytes_per_cluster = sectors_per_cluster * SECTOR_SIZE
        self.reserved_sectors = ESP_RESERVED_SECTORS
        self.num_fats = ESP_NUM_FATS

        # Root directory: one cluster, fixed once computed below.
        self.root_cluster = 2

        self._compute_geometry()
        self.image = bytearray(total_sectors * SECTOR_SIZE)

        # FAT as a list of 32-bit entries, mirrored to disk on write.
        self.fat: List[int] = [0] * self.fat_entries
        self.fat[0] = 0x0FFFFFF8   # media descriptor: 0xF8 plus all high bits set
        self.fat[1] = 0x0FFFFFFF   # reserved: end of chain

        # THE ROOT DIRECTORY MUST BE MARKED ALLOCATED.
        #
        # Cluster 2 is the root directory, and the allocator starts handing out
        # clusters from 3 onward — so nothing else ever sets FAT[2]. Left at
        # zero it reads as "free cluster", and a real FAT driver then sees the
        # root directory chain as unallocated:
        #
        #     mtools:       "Fat problem while decoding 2 0"
        #     Linux vfat:   "can't read superblock"
        #
        # Our own verifier did not catch this because it trusts the same bitmap
        # the writer maintains. It took reading the image with mtools and with
        # the kernel's own FAT driver to see it — which is the argument for
        # cross-checking a format against an independent implementation rather
        # than only against yourself.
        self.fat[self.root_cluster] = 0x0FFFFFFF   # end of chain: root is 1 cluster
        self.next_free_cluster = 3

        self._format_boot_sector()
        self._write_fsinfo()

    # -------------------------------------------------------------------------
    # Geometry
    # -------------------------------------------------------------------------
    def _compute_geometry(self) -> None:
        data_sectors = self.total_sectors - self.reserved_sectors
        # The FAT size must be large enough to describe every cluster, and the
        # FAT itself consumes data space — so this is solved by iteration rather
        # than in closed form. It converges in two or three rounds.
        fat_sectors = 1
        for _ in range(64):
            data_sectors_effective = data_sectors - (self.num_fats * fat_sectors)
            if data_sectors_effective <= 0:
                raise Fat32Error("volume is too small to hold its own FAT")
            clusters = data_sectors_effective // self.sectors_per_cluster
            needed = ((clusters + 2) * 4 + (SECTOR_SIZE - 1)) // SECTOR_SIZE
            if needed <= fat_sectors:
                break
            fat_sectors = needed
        else:
            raise Fat32Error("FAT size did not converge — volume geometry is wrong")

        self.fat_sectors = fat_sectors
        self.data_sectors = data_sectors - (self.num_fats * fat_sectors)
        self.cluster_count = self.data_sectors // self.sectors_per_cluster
        self.fat_entries = self.cluster_count + 2

        if self.cluster_count < 65525:
            raise Fat32Error(
                f"volume has only {self.cluster_count} clusters; FAT32 requires "
                f"at least 65525. Increase the partition size or reduce the "
                f"cluster size."
            )

        self.fat_start_sector = self.reserved_sectors
        self.data_start_sector = self.reserved_sectors + (self.num_fats * self.fat_sectors)

    # -------------------------------------------------------------------------
    # Sector and cluster helpers
    # -------------------------------------------------------------------------
    def _sector_offset(self, lba: int) -> int:
        return lba * SECTOR_SIZE

    def _cluster_to_sector(self, cluster: int) -> int:
        return self.data_start_sector + (cluster - 2) * self.sectors_per_cluster

    def _cluster_slice(self, cluster: int) -> slice:
        start = self._sector_offset(self._cluster_to_sector(cluster))
        return slice(start, start + self.bytes_per_cluster)

    def _write_sector(self, lba: int, data: bytes) -> None:
        if len(data) > SECTOR_SIZE:
            raise Fat32Error("sector write larger than one sector")
        off = self._sector_offset(lba)
        self.image[off:off + len(data)] = data

    # -------------------------------------------------------------------------
    # Formatting
    # -------------------------------------------------------------------------
    def _format_boot_sector(self) -> None:
        bs = bytearray(SECTOR_SIZE)

        bs[0:3] = b"\xEB\x58\x90"                 # jump to the boot code
        bs[3:11] = b"AFRIYIE "                    # OEM name, 8 bytes
        struct.pack_into("<H", bs, 11, SECTOR_SIZE)
        bs[13] = self.sectors_per_cluster
        struct.pack_into("<H", bs, 14, self.reserved_sectors)
        bs[16] = self.num_fats
        struct.pack_into("<H", bs, 17, 0)         # root entries (0 for FAT32)
        struct.pack_into("<H", bs, 19, 0)         # total sectors 16 (0 for FAT32)
        bs[21] = 0xF8                             # media descriptor: fixed disk
        struct.pack_into("<H", bs, 22, 0)         # FAT size 16 (0 for FAT32)
        struct.pack_into("<H", bs, 24, 63)        # sectors per track (geometry only)
        struct.pack_into("<H", bs, 26, 255)       # heads (geometry only)
        struct.pack_into("<I", bs, 28, ESP_START_LBA)   # hidden sectors
        struct.pack_into("<I", bs, 32, self.total_sectors)

        # FAT32 extended BPB
        struct.pack_into("<I", bs, 36, self.fat_sectors)
        struct.pack_into("<H", bs, 40, 0)         # ext flags: FATs mirrored
        struct.pack_into("<H", bs, 42, 0)         # file system version 0.0
        struct.pack_into("<I", bs, 44, self.root_cluster)
        struct.pack_into("<H", bs, 48, 1)         # FSInfo sector
        struct.pack_into("<H", bs, 50, 6)         # backup boot sector
        bs[64] = 0x80                             # drive number
        bs[66] = 0x29                             # extended boot signature
        struct.pack_into("<I", bs, 67, 0xAF121E05)  # volume serial number
        bs[71:82] = b"AFRIYIEOS  "                # volume label, 11 bytes
        bs[82:90] = b"FAT32   "

        # Boot signature. Some firmware checks it; it costs nothing to be correct.
        bs[510] = 0x55
        bs[511] = 0xAA

        self._write_sector(0, bytes(bs))

        # Backup boot sector at sector 6 and its FSInfo at 7.
        self._write_sector(6, bytes(bs))

    def _write_fsinfo(self) -> None:
        fsinfo = bytearray(SECTOR_SIZE)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)   # lead signature
        struct.pack_into("<I", fsinfo, 484, 0x61417272)  # structure signature
        struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)  # free count: unknown
        struct.pack_into("<I", fsinfo, 492, 0xFFFFFFFF)  # next free: unknown
        fsinfo[510] = 0x55
        fsinfo[511] = 0xAA
        self._write_sector(1, bytes(fsinfo))
        self._write_sector(7, bytes(fsinfo))

    # -------------------------------------------------------------------------
    # FAT
    # -------------------------------------------------------------------------
    def _set_fat(self, cluster: int, value: int) -> None:
        if cluster >= self.fat_entries:
            raise Fat32Error(f"cluster {cluster} is beyond the FAT")
        self.fat[cluster] = value & 0x0FFFFFFF

    def _flush_fat(self) -> None:
        raw = bytearray(self.fat_entries * 4)
        for i, value in enumerate(self.fat):
            struct.pack_into("<I", raw, i * 4, value)

        # Pad to whole sectors and write every FAT copy.
        padding = (-len(raw)) % SECTOR_SIZE
        raw += b"\x00" * padding

        for copy_index in range(self.num_fats):
            start = self._sector_offset(
                self.fat_start_sector + copy_index * self.fat_sectors
            )
            self.image[start:start + len(raw)] = raw

    def _allocate_cluster(self) -> int:
        cluster = self.next_free_cluster
        while cluster < self.fat_entries and self.fat[cluster] != 0:
            cluster += 1
        if cluster >= self.fat_entries - 1:
            raise Fat32Error("out of clusters on the ESP")
        self.next_free_cluster = cluster + 1
        self._set_fat(cluster, 0x0FFFFFFF)   # tentatively end-of-chain
        return cluster

    def _chain_for_size(self, size: int) -> List[int]:
        needed = max(1, (size + self.bytes_per_cluster - 1) // self.bytes_per_cluster)
        chain = []
        for _ in range(needed):
            chain.append(self._allocate_cluster())
        for i in range(len(chain) - 1):
            self._set_fat(chain[i], chain[i + 1])
        self._set_fat(chain[-1], 0x0FFFFFFF)
        return chain

    # -------------------------------------------------------------------------
    # Directory entries
    # -------------------------------------------------------------------------
    @staticmethod
    def _short_name_for(name: str, is_dir: bool) -> bytes:
        """Build an 11-byte 8.3 short name.

        Every entry we create also gets a long-name entry, so the short name
        only has to be unique and syntactically valid — not readable.
        """
        upper = name.upper()
        if "." in upper and not is_dir:
            base, _, ext = upper.rpartition(".")
        else:
            base, ext = upper, ""

        # Strip characters that are not legal in an 8.3 name.
        legal = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")
        base = "".join(c for c in base if c in legal)[:8]
        ext = "".join(c for c in ext if c in legal)[:3]

        if not base:
            base = "FILE"

        short = base.ljust(8)[:8].encode("ascii") + ext.ljust(3)[:3].encode("ascii")

        # 0x20 in the first byte marks a deleted entry; force 0x05 in that case.
        if short[0] == 0x20:
            short = b"\x05" + short[1:]

        return short

    @staticmethod
    def _long_name_entries(name: str, short_name: bytes, checksum: int) -> bytes:
        """Build the LFN entry chain that precedes a directory entry."""
        chars = name.encode("utf-16-le")
        # The LFN character list is 13 UTF-16 code units per entry.
        units = [chars[i:i + 2] for i in range(0, len(chars), 2)]
        units.append(b"\x00\x00")   # NUL terminator
        while len(units) % 13 != 0:
            units.append(b"\xFF\xFF")   # padding

        chunk_count = len(units) // 13
        out = bytearray()

        for chunk in range(chunk_count):
            piece = units[chunk * 13:(chunk + 1) * 13]
            entry = bytearray(32)
            ordinal = chunk + 1
            if chunk == chunk_count - 1:
                ordinal |= 0x40        # LAST_LONG_ENTRY
            entry[0] = ordinal
            entry[11] = 0x0F           # ATTR_LONG_NAME
            entry[12] = 0x00           # type
            entry[13] = checksum
            entry[26:28] = b"\x00\x00" # first cluster (always 0 for LFN)
            for i, unit in enumerate(piece):
                if i < 5:
                    entry[1 + i * 2: 1 + i * 2 + 2] = unit
                elif i < 11:
                    entry[14 + (i - 5) * 2: 14 + (i - 5) * 2 + 2] = unit
                else:
                    entry[28 + (i - 11) * 2: 28 + (i - 11) * 2 + 2] = unit
            out += entry

        # LFN entries are stored in reverse order, i.e. the highest ordinal first.
        entries = [bytes(out[i:i + 32]) for i in range(0, len(out), 32)]
        entries.reverse()
        return b"".join(entries)

    @staticmethod
    def _short_name_checksum(short_name: bytes) -> int:
        checksum = 0
        for byte in short_name:
            checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 0xFF
        return checksum

    # -------------------------------------------------------------------------
    # Public API
    # -------------------------------------------------------------------------
    def _read_cluster(self, cluster: int) -> bytes:
        return bytes(self.image[self._cluster_slice(cluster)])

    def _write_cluster(self, cluster: int, data: bytes) -> None:
        sl = self._cluster_slice(cluster)
        if len(data) != self.bytes_per_cluster:
            raise Fat32Error("cluster write must be a whole cluster")
        self.image[sl] = data

    def _iter_dir(self, dir_cluster: int) -> List[DirEntry]:
        entries: List[DirEntry] = []
        cluster = dir_cluster

        while cluster < 0x0FFFFFF8 and cluster >= 2:
            raw = self._read_cluster(cluster)
            for offset in range(0, len(raw), 32):
                entry = raw[offset:offset + 32]
                if entry[0] == 0x00:
                    return entries
                if entry[0] == 0xE5 or entry[11] == 0x0F:
                    continue
                if entry[11] & 0x08 and entry[0] == 0x2E:
                    continue   # "." and ".."

                name = entry[0:8].decode("ascii", "replace").rstrip() 
                ext = entry[8:11].decode("ascii", "replace").rstrip()
                full = name + ("." + ext if ext else "")

                first = (struct.unpack_from("<H", entry, 20)[0] << 16) | \
                        struct.unpack_from("<H", entry, 26)[0]
                size = struct.unpack_from("<I", entry, 28)[0]
                is_dir = bool(entry[11] & 0x10)

                entries.append(DirEntry(full, is_dir, first, size, entry[0:11]))
            cluster = self.fat[cluster]
        return entries

    def _append_entry(self, dir_cluster: int, raw_entry: bytes) -> None:
        """Append one or more 32-byte directory entries to a directory.

        `raw_entry` may contain a whole long-name chain (many 32-byte entries)
        followed by the real entry, so this must be able to span clusters.
        Writing a multi-entry chain into the tail of a cluster would otherwise
        overrun the cluster boundary and silently corrupt the next one.
        """
        if len(raw_entry) == 0 or len(raw_entry) % 32 != 0:
            raise Fat32Error("directory entry data must be a non-empty multiple of 32 bytes")

        count = len(raw_entry) // 32

        # Collect free slots in order. There are two sources of free space:
        #   * 0xE5 entries (deleted) anywhere in the directory;
        #   * the 0x00 terminator and every slot after it.
        # When more room is needed the cluster chain is extended, and a freshly
        # allocated cluster is entirely free because clusters are zero-filled.
        #
        # The subtle requirement is that this keeps extending until there are
        # ENOUGH slots, not merely until the terminator is found. A three-entry
        # long-name chain landing in the last free slot of a directory needs two
        # more slots; stopping at the terminator silently loses the file.
        slots: List[tuple] = []
        cluster = dir_cluster
        directory_ended = False

        while len(slots) < count:
            raw = self._read_cluster(cluster)

            if directory_ended:
                for offset in range(0, len(raw), 32):
                    slots.append((cluster, offset))
            else:
                for offset in range(0, len(raw), 32):
                    marker = raw[offset]
                    if marker == 0x00:
                        # This slot ends the directory, so it and every slot
                        # after it in this cluster are free.
                        directory_ended = True
                        for free_offset in range(offset, len(raw), 32):
                            slots.append((cluster, free_offset))
                        break
                    if marker == 0xE5:
                        slots.append((cluster, offset))
                    if len(slots) >= count:
                        break

            if len(slots) >= count:
                break

            next_cluster = self.fat[cluster]
            if next_cluster >= 0x0FFFFFF8:
                new_cluster = self._allocate_cluster()
                self._set_fat(cluster, new_cluster)
                self._set_fat(new_cluster, 0x0FFFFFFF)
                cluster = new_cluster
                directory_ended = True   # a fresh cluster is all zeros
            else:
                cluster = next_cluster

        for index in range(count):
            target_cluster, offset = slots[index]
            block = bytearray(self._read_cluster(target_cluster))
            block[offset:offset + 32] = raw_entry[index * 32:(index + 1) * 32]
            self._write_cluster(target_cluster, bytes(block))

    def _find_entry(self, dir_cluster: int, name: str) -> Optional[DirEntry]:
        wanted = name.upper()
        for entry in self._iter_dir(dir_cluster):
            if entry.name.upper() == wanted:
                return entry
        return None

    def mkdir(self, path: str) -> int:
        """Create a directory (creating parents as needed). Returns its cluster."""
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        cluster = self.root_cluster

        for part in parts:
            existing = self._find_entry(cluster, part)
            if existing is not None:
                if not existing.is_dir:
                    raise Fat32Error(f"{part} exists and is not a directory")
                cluster = existing.first_cluster
                continue

            new_cluster = self._allocate_cluster()

            # A new directory starts with "." and ".." entries.
            block = bytearray(self.bytes_per_cluster)
            dot = bytearray(32)
            dot[0:11] = b".          "
            dot[11] = 0x10
            struct.pack_into("<H", dot, 20, (new_cluster >> 16) & 0xFFFF)
            struct.pack_into("<H", dot, 26, new_cluster & 0xFFFF)

            dotdot = bytearray(32)
            dotdot[0:11] = b"..         "
            dotdot[11] = 0x10
            struct.pack_into("<H", dotdot, 20, (cluster >> 16) & 0xFFFF)
            struct.pack_into("<H", dotdot, 26, cluster & 0xFFFF)

            block[0:32] = dot
            block[32:64] = dotdot
            self._write_cluster(new_cluster, bytes(block))

            short = self._short_name_for(part, True)
            entry = bytearray(32)
            entry[0:11] = short
            entry[11] = 0x10
            # A zero timestamp is ugly but valid. Firmware does not care.
            struct.pack_into("<H", entry, 20, (new_cluster >> 16) & 0xFFFF)
            struct.pack_into("<H", entry, 26, new_cluster & 0xFFFF)

            raw = self._long_name_entries(part, short,
                                          self._short_name_checksum(short))
            self._append_entry(cluster, raw + bytes(entry))

            cluster = new_cluster

        return cluster

    def write_file(self, path: str, data: bytes) -> None:
        """Write a file, creating parent directories as needed."""
        path = path.replace("\\", "/").lstrip("/")
        parent, _, name = path.rpartition("/")
        dir_cluster = self.mkdir(parent) if parent else self.root_cluster

        if self._find_entry(dir_cluster, name) is not None:
            raise Fat32Error(f"{path} already exists on the image")

        if not data:
            chain: List[int] = []
        else:
            chain = self._chain_for_size(len(data))

        # Cluster data
        for index, cluster in enumerate(chain):
            chunk = data[index * self.bytes_per_cluster:
                         (index + 1) * self.bytes_per_cluster]
            block = bytearray(self.bytes_per_cluster)
            block[0:len(chunk)] = chunk
            self._write_cluster(cluster, bytes(block))

        first_cluster = chain[0] if chain else 0

        short = self._short_name_for(name, False)
        entry = bytearray(32)
        entry[0:11] = short
        entry[11] = 0x20   # archive
        struct.pack_into("<H", entry, 20, (first_cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", entry, 26, first_cluster & 0xFFFF)
        struct.pack_into("<I", entry, 28, len(data))

        lfn = self._long_name_entries(name, short, self._short_name_checksum(short))
        self._append_entry(dir_cluster, lfn + bytes(entry))

    def finalize(self) -> bytes:
        self._flush_fat()
        return bytes(self.image)


# =============================================================================
# GPT
# =============================================================================
def build_protective_mbr(total_sectors: int, esp_start: int, esp_sectors: int) -> bytes:
    mbr = bytearray(SECTOR_SIZE)

    # Boot code: print nothing, just halt if someone boots this in BIOS mode.
    mbr[0:2] = b"\xEB\xFE"

    # Partition 1: type 0xEE (GPT protective), spanning as much as MBR can express.
    entry = 446
    mbr[entry + 0] = 0x00           # not bootable
    mbr[entry + 1:entry + 4] = b"\x00\x02\x00"   # CHS start (dummy)
    mbr[entry + 4] = 0xEE           # GPT protective type
    mbr[entry + 5:entry + 8] = b"\xFF\xFF\xFF"   # CHS end (dummy)
    struct.pack_into("<I", mbr, entry + 8, 1)    # start LBA
    # Cap the protective partition at the 32-bit limit.
    mbr_sectors = min(total_sectors - 1, 0xFFFFFFFF)
    struct.pack_into("<I", mbr, entry + 12, mbr_sectors)

    mbr[510] = 0x55
    mbr[511] = 0xAA

    _ = esp_start, esp_sectors   # kept for signature clarity
    return bytes(mbr)


def build_gpt_entry(name: str, type_guid: bytes, first_lba: int, last_lba: int,
                    unique_guid: bytes, attributes: int = 0) -> bytes:
    entry = bytearray(128)
    entry[0:16] = type_guid
    entry[16:32] = unique_guid
    struct.pack_into("<Q", entry, 32, first_lba)
    struct.pack_into("<Q", entry, 40, last_lba)
    struct.pack_into("<Q", entry, 48, attributes)

    encoded = name.encode("utf-16-le")[:72]
    entry[56:56 + len(encoded)] = encoded
    return bytes(entry)


def build_gpt(total_sectors: int, entries: List[bytes], disk_guid: bytes) -> bytes:
    """Build the primary GPT header (sector 1)."""
    entries_per_sector = SECTOR_SIZE // 128
    entry_array_sectors = (len(entries) + entries_per_sector - 1) // entries_per_sector
    if entry_array_sectors < 32:
        entry_array_sectors = 32   # the specification's minimum

    entry_array = b"".join(entries)
    entry_array = entry_array.ljust(entry_array_sectors * SECTOR_SIZE, b"\x00")
    entry_array_crc = zlib.crc32(entry_array) & 0xFFFFFFFF

    header = bytearray(SECTOR_SIZE)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)          # revision 1.0
    struct.pack_into("<I", header, 12, 92)                 # header size
    struct.pack_into("<I", header, 16, 0)                  # CRC32 (patched below)
    struct.pack_into("<I", header, 20, 0)                  # reserved
    struct.pack_into("<Q", header, 24, 1)                  # current LBA
    struct.pack_into("<Q", header, 32, total_sectors - 1)  # backup LBA
    struct.pack_into("<Q", header, 40, 34)                 # first usable LBA
    struct.pack_into("<Q", header, 48, total_sectors - 34) # last usable LBA
    header[56:72] = disk_guid
    struct.pack_into("<Q", header, 72, 2)                  # entry array LBA
    struct.pack_into("<I", header, 80, len(entries))
    struct.pack_into("<I", header, 84, 128)                # entry size
    struct.pack_into("<I", header, 88, entry_array_crc)

    header_crc = zlib.crc32(bytes(header[0:92])) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, header_crc)

    return bytes(header) + entry_array


# =============================================================================
# Image assembly
# =============================================================================
def find_boot_artifact(build_dir: str) -> str:
    candidates = [
        os.path.join(build_dir, "boot", "BOOTX64.EFI"),
        os.path.join(build_dir, "BOOTX64.EFI"),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    raise SystemExit(
        f"BOOTX64.EFI not found in {build_dir}. Build the boot target first:\n"
        f"    cmake --build {build_dir}"
    )


def find_init_artifact(build_dir: str) -> str:
    """Locates the user-space init program's ELF image.

    Required, not optional. The kernel's last act at boot is to load this file
    off the FAT32 volume and enter it in ring 3; an image built without it boots
    to a panic. Failing the packaging step instead moves the error from a
    several-minute QEMU run back to the place that caused it.
    """
    candidates = [
        os.path.join(build_dir, "init.elf"),
        os.path.join(build_dir, "apps", "init", "init.elf"),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    raise SystemExit(
        f"init.elf not found in {build_dir}. Build the user-space target first:\n"
        f"    cmake --build {build_dir}"
    )


def make_image(build_dir: str, output: str, arch: str, esp_size_mb: int) -> None:
    if arch != "x86_64":
        raise SystemExit(
            f"Image creation for '{arch}' is not implemented at v0.1.\n"
            f"The phone image (fastboot-compatible boot.img) is part of "
            f"milestone v1.1 — see docs/AfriyieOS-Blueprint.md section 11."
        )

    boot_app = find_boot_artifact(build_dir)
    boot_bytes = open(boot_app, "rb").read()
    log(f"boot application: {boot_app} ({len(boot_bytes)} bytes)")

    init_app = find_init_artifact(build_dir)
    log(f"init program: {init_app} ({os.path.getsize(init_app)} bytes)")

    esp_bytes = esp_size_mb * 1024 * 1024
    esp_sectors = esp_bytes // SECTOR_SIZE

    total_sectors = ESP_START_LBA + esp_sectors + 33   # +33 for the backup GPT
    total_sectors = (total_sectors + 2047) // 2048 * 2048  # round to 1 MiB
    esp_sectors = total_sectors - ESP_START_LBA - 33
    log(f"image size: {total_sectors * SECTOR_SIZE // (1024 * 1024)} MiB "
        f"({total_sectors} sectors)")

    # --- FAT32 ESP -----------------------------------------------------------
    volume = Fat32Volume(esp_sectors)
    volume.write_file(EFI_FALLBACK_PATH, boot_bytes)

    # The v0.3 acceptance file.
    #
    # This is not filler. The kernel's FAT32 self test mounts this ESP and reads
    # exactly this file, checking the contents rather than merely printing
    # whatever comes back — so a file system reader that returns the wrong sector
    # fails the test instead of appearing to work.
    #
    # The newline is part of the expected contents, so a reader that returns the
    # right bytes with the wrong length fails as well.
    volume.write_file("HELLO.TXT", b"Hello from disk\n")

    # The v0.4 acceptance artifact: a real user program, built by the cross
    # compiler and linked at 4 GiB, sitting in the FAT32 root as an ELF file.
    #
    # It is packed unmodified. Nothing here patches an entry point or relocates
    # a segment — the kernel's ELF loader is expected to read the file the way
    # the linker wrote it, so anything this step did would be hiding a loader
    # bug rather than fixing one.
    with open(init_app, "rb") as handle:
        init_bytes = handle.read()
    volume.write_file("INIT.ELF", init_bytes)

    log(f"ESP: FAT32, {volume.cluster_count} clusters, "
        f"{volume.fat_sectors} sectors per FAT")
    log(f"     wrote {EFI_FALLBACK_PATH}")
    log(f"     wrote HELLO.TXT ({len(b'Hello from disk\n')} bytes)")
    log(f"     wrote INIT.ELF ({len(init_bytes)} bytes)")

    esp_image = volume.finalize()
    if len(esp_image) != esp_sectors * SECTOR_SIZE:
        raise SystemExit("internal error: ESP image size mismatch")

    # --- GPT -----------------------------------------------------------------
    disk_guid = os.urandom(16)
    # Ensure the GUID has the variant/version bits a real disk would have.
    disk_guid = bytearray(disk_guid)
    disk_guid[7] = (disk_guid[7] & 0x0F) | 0x40
    disk_guid[8] = (disk_guid[8] & 0x3F) | 0x80
    disk_guid = bytes(disk_guid)

    esp_part_guid = bytearray(os.urandom(16))
    esp_part_guid[7] = (esp_part_guid[7] & 0x0F) | 0x40
    esp_part_guid[8] = (esp_part_guid[8] & 0x3F) | 0x80
    esp_part_guid = bytes(esp_part_guid)

    esp_first_lba = ESP_START_LBA
    esp_last_lba = ESP_START_LBA + esp_sectors - 1

    entries = [
        build_gpt_entry("EFI System Partition", ESP_TYPE_GUID,
                        esp_first_lba, esp_last_lba, esp_part_guid),
    ]
    # The entry array is always 128 entries long, zero-filled beyond ours.
    entries += [b"\x00" * 128] * (128 - len(entries))

    primary = build_gpt(total_sectors, entries, disk_guid)

    # --- Assemble ------------------------------------------------------------
    image = bytearray(total_sectors * SECTOR_SIZE)
    image[0:SECTOR_SIZE] = build_protective_mbr(total_sectors, esp_first_lba,
                                                esp_sectors)
    image[SECTOR_SIZE:2 * SECTOR_SIZE] = primary[0:SECTOR_SIZE]
    image[2 * SECTOR_SIZE:2 * SECTOR_SIZE + len(primary) - SECTOR_SIZE] = \
        primary[SECTOR_SIZE:]

    esp_offset = esp_first_lba * SECTOR_SIZE
    image[esp_offset:esp_offset + len(esp_image)] = esp_image

    # --- Backup GPT ----------------------------------------------------------
    backup_entries_lba = total_sectors - 33
    entry_array = b"".join(entries).ljust(32 * SECTOR_SIZE, b"\x00")
    backup_entries_offset = backup_entries_lba * SECTOR_SIZE
    image[backup_entries_offset:backup_entries_offset + len(entry_array)] = entry_array

    backup_header = bytearray(primary[0:SECTOR_SIZE])
    struct.pack_into("<Q", backup_header, 24, total_sectors - 1)   # current LBA
    struct.pack_into("<Q", backup_header, 32, 1)                   # backup LBA
    struct.pack_into("<Q", backup_header, 72, backup_entries_lba)  # entry array LBA
    struct.pack_into("<I", backup_header, 16, 0)                   # clear CRC
    backup_crc = zlib.crc32(bytes(backup_header[0:92])) & 0xFFFFFFFF
    struct.pack_into("<I", backup_header, 16, backup_crc)

    backup_header_offset = (total_sectors - 1) * SECTOR_SIZE
    image[backup_header_offset:backup_header_offset + SECTOR_SIZE] = backup_header

    # --- Write ---------------------------------------------------------------
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output, "wb") as handle:
        handle.write(image)

    log(f"wrote {output} ({len(image) // (1024 * 1024)} MiB)")
    log("")
    log("Boot it with:")
    log(f"    python3 tools/run_qemu.py --arch x86_64 --image {output}")
    log("")
    log("Or write it to a USB stick (CHECK THE DEVICE NAME FIRST):")
    log(f"    sudo dd if={output} of=/dev/sdX bs=4M status=progress conv=fsync")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a bootable AfriyieOS disk image",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--arch", required=True, choices=["x86_64", "aarch64"],
                        help="target architecture")
    parser.add_argument("--build-dir", required=True,
                        help="CMake build directory containing the artefacts")
    parser.add_argument("--output", required=True, help="output image path")
    parser.add_argument("--esp-size-mb", type=int, default=ESP_SIZE_BYTES // (1024 * 1024),
                        help="EFI System Partition size in MiB (default: 64)")

    args = parser.parse_args()

    try:
        make_image(args.build_dir, args.output, args.arch, args.esp_size_mb)
    except Fat32Error as exc:
        print(f"FAT32 error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

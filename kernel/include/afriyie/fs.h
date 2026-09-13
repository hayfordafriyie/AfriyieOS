// SPDX-License-Identifier: MIT
// AfriyieOS — partition tables and file system interfaces

#ifndef AFRIYIE_FS_H
#define AFRIYIE_FS_H

#include "types.h"
#include "status.h"
#include "block.h"

// =============================================================================
// Partition tables
// =============================================================================

typedef struct {
    af_u64 start_lba;      // first sector of the partition
    af_u64 sector_count;
    char   name[37];       // UTF-8, NUL-terminated
    af_u8  type_guid[16];
} af_partition_t;

#define AF_MAX_PARTITIONS 8

typedef struct {
    af_partition_t entries[AF_MAX_PARTITIONS];
    af_u32         count;
    bool           is_gpt;      // false when only a protective MBR is present
} af_partition_table_t;

// Reads the partition table from a block device. Handles GPT, and detects the
// MBR-only case so the caller can report it rather than silently finding nothing.
af_status_t partition_table_read(af_block_device_t *dev,
                                 af_partition_table_t *out);

// The EFI System Partition, identified by its type GUID. Returns AF_ERR_NOENT
// when there is no ESP, which is a normal outcome on a non-boot disk.
af_status_t partition_find_esp(const af_partition_table_t *table,
                               af_partition_t *out);

void partition_dump(const af_partition_table_t *table);

// =============================================================================
// FAT32 (read-only)
//
// Read-only is deliberate at v0.3. Writing to a file system is where the
// interesting failure modes live — allocation, truncation, crash consistency —
// and none of them can be tested properly until there is a file system we
// control end to end. AFS (v0.9) is that file system; FAT32 stays a reader, which
// is all the kernel needs to load a ramdisk or a second-stage binary.
// =============================================================================

#define AF_NAME_MAX     256
#define AF_FAT_MAX_OPEN 16

typedef struct {
    af_block_device_t *dev;
    af_u64 partition_start;     // LBA of the partition on the device
    af_u32 bytes_per_sector;
    af_u32 sectors_per_cluster;
    af_u32 reserved_sectors;
    af_u32 num_fats;
    af_u32 fat_sectors;
    af_u32 root_cluster;
    af_u32 bytes_per_cluster;
    af_u64 fat_start_lba;
    af_u64 data_start_lba;
    af_u32 cluster_count;
    bool   mounted;
} fat32_volume_t;

typedef struct {
    af_u32 first_cluster;
    af_u32 size;
    bool   is_directory;
    char   name[AF_NAME_MAX];
} fat32_entry_t;

typedef struct {
    bool   in_use;
    fat32_volume_t *volume;
    fat32_entry_t  entry;
    af_u32 offset;             // read cursor
} fat32_file_t;

// Mounts a FAT32 volume from a partition. Validates the BPB thoroughly: a
// malformed BPB is far more common than a missing one, and a parser that trusts
// it walks off the end of the disk.
af_status_t fat32_mount(fat32_volume_t *vol, af_block_device_t *dev,
                        const af_partition_t *part);

void fat32_dump_info(const fat32_volume_t *vol);

// Resolves a path such as "/EFI/BOOT/BOOTX64.EFI". Case-insensitive, as FAT is.
// Both '/' and '\\' are accepted as separators, because a path written by a
// Windows tool and one written by hand are the same path to FAT.
af_status_t fat32_lookup(fat32_volume_t *vol, const char *path,
                         fat32_entry_t *out);

// Lists entry `index` of a directory. Returns AF_ERR_NOENT past the end, which
// is the normal way to iterate.
af_status_t fat32_readdir(fat32_volume_t *vol, af_u32 dir_cluster,
                          af_u32 index, fat32_entry_t *out);

af_status_t fat32_open(fat32_volume_t *vol, const char *path, fat32_file_t **out);
af_status_t fat32_read(fat32_file_t *file, void *buffer, af_u32 bytes,
                       af_u32 *bytes_read);
af_status_t fat32_close(fat32_file_t *file);

// Reads a whole file into a caller-provided buffer. Refuses when the file is
// larger than the buffer rather than truncating silently — a truncated read of
// an executable is a far worse failure than a refused one.
af_status_t fat32_read_file(fat32_volume_t *vol, const char *path,
                            void *buffer, af_u32 buffer_size,
                            af_u32 *bytes_read);

// The v0.3 acceptance test: mount the boot disk's ESP, read a file from it and
// print its contents.
void fat32_selftest(void);

#endif // AFRIYIE_FS_H

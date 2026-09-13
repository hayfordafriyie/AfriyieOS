// SPDX-License-Identifier: MIT
// AfriyieOS — FAT32, read-only
//
// =============================================================================
// WHY READ-ONLY, AND WHY THAT IS NOT A SHORTCUT
// =============================================================================
// Writing to a file system is where the interesting failure modes live:
// allocation, truncation, and crash consistency. None of them can be tested
// properly until there is a file system we control end to end, and FAT32's
// allocation rules are just awkward enough to make it a poor training ground.
//
// So FAT32 is a reader. It is what the kernel needs to load a ramdisk or a
// second-stage binary, and it is deliberately not more than that. AFS (v0.9) is
// the read-write file system, and it will be built with a journal and a
// crash-consistency test from the start rather than being retrofitted.
// =============================================================================

#include "afriyie/fs.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

#define SECTOR_SIZE 512

#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_LONG_NAME  0x0F   // the low four bits all set

#define FAT32_EOC             0x0FFFFFF8   // end of chain, or beyond
#define FAT32_FREE            0x00000000
#define FAT32_BAD             0x0FFFFFF7

static af_u32 rd16(const af_u8 *p) { return (af_u32)p[0] | ((af_u32)p[1] << 8); }

static af_u32 rd32(const af_u8 *p)
{
    return (af_u32)p[0] | ((af_u32)p[1] << 8) |
           ((af_u32)p[2] << 16) | ((af_u32)p[3] << 24);
}

static bool is_power_of_two(af_u32 v) { return v != 0 && (v & (v - 1)) == 0; }

// -----------------------------------------------------------------------------
// Mounting
// -----------------------------------------------------------------------------
af_status_t fat32_mount(fat32_volume_t *vol, af_block_device_t *dev,
                        const af_partition_t *part)
{
    if (vol == NULL || dev == NULL || part == NULL) {
        return AF_ERR_INVAL;
    }

    af_memset(vol, 0, sizeof(*vol));

    vol->dev = dev;
    vol->partition_start = part->start_lba;

    static af_u8 bpb[SECTOR_SIZE];

    af_status_t rc = block_read(dev, part->start_lba, 1, bpb);
    if (af_status_err(rc)) {
        return rc;
    }

    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        af_warn("fat32", "no 0x55AA signature in the boot sector");
        return AF_ERR_FS_CORRUPT;
    }

    vol->bytes_per_sector   = rd16(bpb + 11);
    vol->sectors_per_cluster = bpb[13];
    vol->reserved_sectors   = rd16(bpb + 14);
    vol->num_fats           = bpb[16];
    vol->fat_sectors        = rd32(bpb + 36);
    vol->root_cluster       = rd32(bpb + 44);

    // =========================================================================
    // VALIDATE EVERY FIELD THE PARSER WILL MULTIPLY BY
    // =========================================================================
    // A corrupt or hostile BPB is far more common than a missing one, and every
    // one of these values is used in arithmetic that produces a sector number.
    // A single bad field turns a read into a walk off the end of the disk, which
    // the block layer will catch — but by then the error names the wrong thing.
    //
    // Checking here means a malformed volume is rejected at mount with a reason,
    // rather than producing a confusing failure three layers down.
    // =========================================================================
    if (vol->bytes_per_sector != 512 && vol->bytes_per_sector != 1024 &&
        vol->bytes_per_sector != 2048 && vol->bytes_per_sector != 4096) {
        af_error("fat32", "invalid bytes per sector: %u", vol->bytes_per_sector);
        return AF_ERR_FS_CORRUPT;
    }

    // The cluster size must be a power of two, between 1 and 128 sectors. FAT
    // requires it, and a non-power-of-two would break the shift arithmetic every
    // cluster calculation depends on.
    if (!is_power_of_two(vol->sectors_per_cluster) ||
        vol->sectors_per_cluster > 128) {
        af_error("fat32", "invalid sectors per cluster: %u",
                 vol->sectors_per_cluster);
        return AF_ERR_FS_CORRUPT;
    }

    if (vol->reserved_sectors == 0) {
        af_error("fat32", "reserved sector count is 0");
        return AF_ERR_FS_CORRUPT;
    }

    if (vol->num_fats == 0 || vol->num_fats > 4) {
        af_error("fat32", "invalid FAT count: %u", vol->num_fats);
        return AF_ERR_FS_CORRUPT;
    }

    if (vol->fat_sectors == 0) {
        af_error("fat32", "FAT size is 0 — this is not a FAT32 volume "
                          "(a FAT16 volume has FATSz32 = 0)");
        return AF_ERR_FS_CORRUPT;
    }

    if (vol->root_cluster < 2) {
        af_error("fat32", "root cluster %u is below the first data cluster",
                 vol->root_cluster);
        return AF_ERR_FS_CORRUPT;
    }

    vol->bytes_per_cluster = vol->bytes_per_sector * vol->sectors_per_cluster;
    vol->fat_start_lba  = part->start_lba + vol->reserved_sectors;
    vol->data_start_lba = vol->fat_start_lba +
                          (af_u64)vol->num_fats * vol->fat_sectors;

    // Cluster count, used to bound every chain walk. Counting from the
    // partition size rather than trusting a field means a lying BPB cannot make
    // the walker run past the end.
    af_u64 data_sectors = part->sector_count - vol->reserved_sectors -
                          (af_u64)vol->num_fats * vol->fat_sectors;
    vol->cluster_count = (af_u32)(data_sectors / vol->sectors_per_cluster);

    if (vol->cluster_count < 65525) {
        af_error("fat32", "only %u clusters — that is FAT16 or FAT12, not FAT32 "
                          "(FAT32 needs at least 65525)",
                 vol->cluster_count);
        return AF_ERR_FS_CORRUPT;
    }

    vol->mounted = true;

    return AF_OK;
}

void fat32_dump_info(const fat32_volume_t *vol)
{
    if (vol == NULL || !vol->mounted) {
        return;
    }

    af_info("fat32", "mounted at LBA %llu: %u bytes/sector, %u sectors/cluster "
                     "(%u byte clusters)",
            (unsigned long long)vol->partition_start, vol->bytes_per_sector,
            vol->sectors_per_cluster, vol->bytes_per_cluster);
    af_info("fat32", "  FAT at LBA %llu (%u sectors x %u copies), data at LBA "
                     "%llu, root cluster %u, %u clusters",
            (unsigned long long)vol->fat_start_lba, vol->fat_sectors,
            vol->num_fats, (unsigned long long)vol->data_start_lba,
            vol->root_cluster, vol->cluster_count);
}

// -----------------------------------------------------------------------------
// Cluster access
// -----------------------------------------------------------------------------
static af_u64 cluster_to_lba(const fat32_volume_t *vol, af_u32 cluster)
{
    return vol->data_start_lba +
           (af_u64)(cluster - 2) * vol->sectors_per_cluster;
}

// Reads the next cluster in a chain, or FAT32_EOC at the end.
static af_u32 fat_next_cluster(const fat32_volume_t *vol, af_u32 cluster)
{
    af_u64 fat_offset = (af_u64)cluster * 4;
    af_u64 sector = vol->fat_start_lba + (fat_offset / vol->bytes_per_sector);
    af_u32 offset = (af_u32)(fat_offset % vol->bytes_per_sector);

    static af_u8 sector_data[SECTOR_SIZE];

    if (af_status_err(block_read((af_block_device_t *)vol->dev, sector, 1,
                                 sector_data))) {
        return FAT32_EOC;
    }

    // Only the low 28 bits are the cluster number; the top four are reserved and
    // some tools leave them set. Masking is not optional — an unmasked value
    // walks the chain to a cluster number that does not exist.
    return rd32(sector_data + offset) & 0x0FFFFFFF;
}

static af_status_t read_cluster(const fat32_volume_t *vol, af_u32 cluster,
                                af_u8 *buffer)
{
    if (cluster < 2 || cluster >= vol->cluster_count + 2) {
        af_error("fat32", "cluster %u is out of range (2..%u)",
                 cluster, vol->cluster_count + 2);
        return AF_ERR_FS_CORRUPT;
    }

    return block_read((af_block_device_t *)vol->dev,
                      cluster_to_lba(vol, cluster),
                      vol->sectors_per_cluster, buffer);
}

// -----------------------------------------------------------------------------
// Directory entries
// -----------------------------------------------------------------------------
//
// Long file names are stored as a chain of entries immediately BEFORE the 8.3
// entry they belong to, in REVERSE order, with the highest-numbered fragment
// first. Reassembling them means collecting the fragments and reversing — and
// checking the checksum byte, which ties the fragments to the entry. Without the
// checksum a directory edited by an unaware tool produces a name assembled from
// somebody else's fragments, which is worse than no name at all.
static af_u8 short_name_checksum(const af_u8 *short_name)
{
    af_u8 sum = 0;
    for (af_u32 i = 0; i < 11; i++) {
        sum = (af_u8)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

af_status_t fat32_readdir(fat32_volume_t *vol, af_u32 dir_cluster,
                          af_u32 index, fat32_entry_t *out)
{
    if (vol == NULL || !vol->mounted || out == NULL) {
        return AF_ERR_INVAL;
    }

    static af_u8 cluster_data[64 * AF_KIB];
    if (vol->bytes_per_cluster > sizeof(cluster_data)) {
        af_error("fat32", "cluster size %u exceeds the %u-byte read buffer",
                 vol->bytes_per_cluster, (af_u32)sizeof(cluster_data));
        return AF_ERR_NOTSUP;
    }

    af_u32 found = 0;
    af_u32 cluster = dir_cluster;
    af_u32 guard = 0;

    // Long-name fragments, collected as we walk.
    char   lfn[AF_NAME_MAX];
    af_u32 lfn_length = 0;
    af_u8  lfn_checksum = 0;
    bool   have_lfn = false;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (++guard > vol->cluster_count) {
            af_error("fat32", "directory chain at cluster %u is cyclic",
                     dir_cluster);
            return AF_ERR_FS_CORRUPT;
        }

        af_status_t rc = read_cluster(vol, cluster, cluster_data);
        if (af_status_err(rc)) {
            return rc;
        }

        for (af_u32 offset = 0; offset < vol->bytes_per_cluster; offset += 32) {
            const af_u8 *e = cluster_data + offset;

            // 0x00 means "no more entries in this directory".
            if (e[0] == 0x00) {
                return AF_ERR_NOENT;
            }
            // 0xE5 marks a deleted entry.
            if (e[0] == 0xE5) {
                have_lfn = false;
                lfn_length = 0;
                continue;
            }

            af_u8 attr = e[11];

            if (attr == FAT32_ATTR_LONG_NAME) {
                // Fragments arrive highest-ordinal first. Build the string by
                // prepending each fragment, which reassembles the name in order
                // without a second pass.
                af_u32 ordinal = e[0] & 0x1F;
                if (ordinal == 0 || ordinal > 20) {
                    have_lfn = false;
                    lfn_length = 0;
                    continue;
                }

                // Reset the accumulator for a new name.
                //
                // Clearing lfn_length is a LOGICAL reset; the buffer still holds
                // the previous name's characters. The prepend below shifts the
                // existing content right to make room, and what it shifts is
                // whatever is in the buffer — including the byte it will treat as
                // the terminator.
                //
                // Without this line the terminator shifted into place is the
                // PREVIOUS name's first character, so every name after the first
                // absorbs stale bytes until it happens to hit a zero:
                //
                //     /EFIeP/        instead of /EFI/
                //     /HELLO.TXTE    instead of /HELLO.TXT
                //
                // The sizes were correct throughout, which is what pointed at the
                // name assembly rather than at the directory walk.
                if (lfn_length == 0) {
                    lfn[0] = '\0';
                }

                char fragment[14];
                af_u32 n = 0;

                // The 13 UTF-16 code units are split across the entry in three
                // runs, which is a layout quirk worth writing out rather than
                // computing: units 0-4 at offset 1, 5-10 at 14, 11-12 at 28.
                for (af_u32 u = 0; u < 13; u++) {
                    af_u32 pos;
                    if (u < 5)       { pos = 1 + u * 2; }
                    else if (u < 11) { pos = 14 + (u - 5) * 2; }
                    else             { pos = 28 + (u - 11) * 2; }

                    af_u16 unit = (af_u16)(e[pos] | (e[pos + 1] << 8));
                    if (unit == 0 || unit == 0xFFFF) {
                        break;
                    }
                    // Only ASCII is represented; anything else becomes '?'
                    // rather than being mangled into invalid UTF-8.
                    fragment[n++] = (unit < 128) ? (char)unit : '?';
                }
                fragment[n] = '\0';

                af_size frag_len = af_strlen(fragment);
                if (frag_len + lfn_length + 1 < sizeof(lfn)) {
                    af_memmove(lfn + frag_len, lfn, lfn_length + 1);
                    af_memcpy(lfn, fragment, frag_len);
                    lfn_length += frag_len;
                    lfn_checksum = e[13];
                    have_lfn = true;
                }
                continue;
            }

            if ((attr & FAT32_ATTR_VOLUME_ID) != 0 && (attr & FAT32_ATTR_DIRECTORY) == 0) {
                continue;   // the volume label is not a file
            }

            // --- a real entry ---
            if (found == index) {
                af_u32 first = ((af_u32)rd16(e + 20) << 16) | rd16(e + 26);

                out->first_cluster = first;
                out->size = rd32(e + 28);
                out->is_directory = (attr & FAT32_ATTR_DIRECTORY) != 0;

                // The "." and ".." entries are real directories with one-character
                // names and must not be presented as files.
                if (e[0] == '.' ) {
                    out->name[0] = '.';
                    out->name[1] = (e[1] == '.') ? '.' : '\0';
                    out->name[2] = '\0';
                } else if (have_lfn && lfn_checksum == short_name_checksum(e)) {
                    af_strlcpy(out->name, lfn, sizeof(out->name));
                } else {
                    // No usable long name: build the 8.3 name.
                    char base[9];
                    char ext[4];
                    af_u32 n = 0;
                    for (af_u32 i = 0; i < 8 && e[i] != ' '; i++) {
                        base[n++] = (char)e[i];
                    }
                    base[n] = '\0';
                    n = 0;
                    for (af_u32 i = 8; i < 11 && e[i] != ' '; i++) {
                        ext[n++] = (char)e[i];
                    }
                    ext[n] = '\0';

                    if (ext[0] != '\0') {
                        af_snprintf(out->name, sizeof(out->name), "%s.%s",
                                    base, ext);
                    } else {
                        af_strlcpy(out->name, base, sizeof(out->name));
                    }
                }

                return AF_OK;
            }

            found++;
            have_lfn = false;
            lfn_length = 0;
        }

        cluster = fat_next_cluster(vol, cluster);
    }

    return AF_ERR_NOENT;
}

// -----------------------------------------------------------------------------
// Path resolution
// -----------------------------------------------------------------------------
af_status_t fat32_lookup(fat32_volume_t *vol, const char *path,
                         fat32_entry_t *out)
{
    if (vol == NULL || !vol->mounted || path == NULL || out == NULL) {
        return AF_ERR_INVAL;
    }

    af_u32 cluster = vol->root_cluster;
    const char *cursor = path;

    // Skip leading separators. Both '/' and '\\' are accepted: FAT itself does
    // not treat them as different, so a path written by a Windows tool and one
    // written by hand address the same file.
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }

    if (*cursor == '\0') {
        // The root directory itself.
        out->first_cluster = vol->root_cluster;
        out->is_directory = true;
        out->size = 0;
        af_strlcpy(out->name, "/", sizeof(out->name));
        return AF_OK;
    }

    fat32_entry_t entry;
    af_u32 guard = 0;

    while (*cursor != '\0') {
        char component[AF_NAME_MAX];
        af_u32 n = 0;

        while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
            if (n + 1 < sizeof(component)) {
                component[n++] = *cursor;
            }
            cursor++;
        }
        component[n] = '\0';

        while (*cursor == '/' || *cursor == '\\') {
            cursor++;
        }

        if (n == 0) {
            break;
        }

        // Linear search of the directory. FAT has no index for directory
        // lookups, so this is what every implementation does; the directories a
        // kernel walks at boot are small.
        bool found = false;
        for (af_u32 i = 0; ; i++) {
            af_status_t rc = fat32_readdir(vol, cluster, i, &entry);
            if (rc == AF_ERR_NOENT) {
                break;
            }
            if (af_status_err(rc)) {
                return rc;
            }

            // FAT file names are case-insensitive.
            if (af_strcmp(entry.name, component) == 0) {
                found = true;
                break;
            }
            // ASCII case-insensitive fallback.
            af_size len = af_strlen(entry.name);
            if (len == af_strlen(component)) {
                bool same = true;
                for (af_size k = 0; k < len; k++) {
                    char a = entry.name[k];
                    char b = component[k];
                    if (a >= 'a' && a <= 'z') { a = (char)(a - 32); }
                    if (b >= 'a' && b <= 'z') { b = (char)(b - 32); }
                    if (a != b) { same = false; break; }
                }
                if (same) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            return AF_ERR_NOENT;
        }

        if (*cursor == '\0') {
            *out = entry;
            return AF_OK;
        }

        if (!entry.is_directory) {
            return AF_ERR_NOTDIR;
        }

        cluster = entry.first_cluster;

        if (++guard > 32) {
            af_error("fat32", "path '%s' is nested more than 32 levels deep",
                     path);
            return AF_ERR_INVAL;
        }
    }

    return AF_ERR_NOENT;
}

// -----------------------------------------------------------------------------
// Reading files
// -----------------------------------------------------------------------------
static fat32_file_t s_open_files[AF_FAT_MAX_OPEN];

af_status_t fat32_open(fat32_volume_t *vol, const char *path, fat32_file_t **out)
{
    if (out == NULL) {
        return AF_ERR_INVAL;
    }

    fat32_entry_t entry;
    af_status_t rc = fat32_lookup(vol, path, &entry);
    if (af_status_err(rc)) {
        return rc;
    }

    if (entry.is_directory) {
        return AF_ERR_ISDIR;
    }

    for (af_u32 i = 0; i < AF_FAT_MAX_OPEN; i++) {
        if (s_open_files[i].in_use) {
            continue;
        }

        s_open_files[i].in_use  = true;
        s_open_files[i].volume  = vol;
        s_open_files[i].entry   = entry;
        s_open_files[i].offset  = 0;

        *out = &s_open_files[i];
        return AF_OK;
    }

    af_error("fat32", "no free file handles (limit %u)", (af_u32)AF_FAT_MAX_OPEN);
    return AF_ERR_TOOMANY;
}

af_status_t fat32_read(fat32_file_t *file, void *buffer, af_u32 bytes,
                       af_u32 *bytes_read)
{
    if (file == NULL || !file->in_use || buffer == NULL) {
        return AF_ERR_INVAL;
    }

    fat32_volume_t *vol = file->volume;

    if (bytes_read != NULL) {
        *bytes_read = 0;
    }

    if (file->offset >= file->entry.size) {
        return AF_OK;   // at the end, which is not an error
    }

    af_u32 remaining = file->entry.size - file->offset;
    if (bytes > remaining) {
        bytes = remaining;
    }

    static af_u8 cluster_data[64 * AF_KIB];

    af_u32 done = 0;
    af_u32 cluster = file->entry.first_cluster;
    af_u32 skip = file->offset / vol->bytes_per_cluster;
    af_u32 within = file->offset % vol->bytes_per_cluster;

    // Skip whole clusters to reach the read position.
    for (af_u32 i = 0; i < skip; i++) {
        if (cluster < 2 || cluster >= FAT32_EOC) {
            return AF_ERR_FS_CORRUPT;
        }
        cluster = fat_next_cluster(vol, cluster);
    }

    af_u8 *out = (af_u8 *)buffer;
    af_u32 guard = 0;

    while (done < bytes) {
        if (cluster < 2 || cluster >= FAT32_EOC) {
            af_error("fat32", "chain ended after %u of %u bytes",
                     done, bytes);
            return AF_ERR_FS_CORRUPT;
        }
        if (++guard > vol->cluster_count) {
            return AF_ERR_FS_CORRUPT;
        }

        af_status_t rc = read_cluster(vol, cluster, cluster_data);
        if (af_status_err(rc)) {
            return rc;
        }

        af_u32 available = vol->bytes_per_cluster - within;
        af_u32 take = bytes - done;
        if (take > available) {
            take = available;
        }

        af_memcpy(out + done, cluster_data + within, take);
        done += take;
        within = 0;

        if (done < bytes) {
            cluster = fat_next_cluster(vol, cluster);
        }
    }

    file->offset += done;

    if (bytes_read != NULL) {
        *bytes_read = done;
    }

    return AF_OK;
}

af_status_t fat32_close(fat32_file_t *file)
{
    if (file == NULL || !file->in_use) {
        return AF_ERR_INVAL;
    }
    file->in_use = false;
    return AF_OK;
}

af_status_t fat32_read_file(fat32_volume_t *vol, const char *path,
                            void *buffer, af_u32 buffer_size,
                            af_u32 *bytes_read)
{
    fat32_entry_t entry;
    af_status_t rc = fat32_lookup(vol, path, &entry);
    if (af_status_err(rc)) {
        return rc;
    }

    if (entry.is_directory) {
        return AF_ERR_ISDIR;
    }

    if (entry.size > buffer_size) {
        // Refusing rather than truncating: a truncated read of an executable is
        // a far worse failure than a refused one, because it produces something
        // that looks like it worked.
        af_error("fat32", "'%s' is %u bytes but the buffer is only %u",
                 path, entry.size, buffer_size);
        return AF_ERR_OVERFLOW;
    }

    fat32_file_t *file = NULL;
    rc = fat32_open(vol, path, &file);
    if (af_status_err(rc)) {
        return rc;
    }

    af_u32 read = 0;
    rc = fat32_read(file, buffer, entry.size, &read);
    fat32_close(file);

    if (af_status_err(rc)) {
        return rc;
    }

    if (read != entry.size) {
        af_error("fat32", "'%s' read %u of %u bytes", path, read, entry.size);
        return AF_ERR_IO;
    }

    if (bytes_read != NULL) {
        *bytes_read = read;
    }

    return AF_OK;
}

// =============================================================================
// Self test — the v0.3 acceptance criterion
// =============================================================================
//
// "Read a file containing `Hello from disk` from the disk and print it."
//
// Everything below this line in the stack has to work for the test to pass: the
// block device read sector 0, the GPT parser found the EFI System Partition, the
// FAT32 mount validated the BPB, the directory walk reassembled a long file name
// from its fragments, the cluster chain walker followed the FAT, and the file
// contents arrived intact.
//
// The test checks the contents against the exact expected string rather than
// merely printing whatever came back. Printing whatever arrives is how a test
// that reads the wrong sector passes.
void fat32_selftest(void)
{
    af_block_device_t *dev = block_boot_device();
    if (dev == NULL) {
        af_warn("test", "  skip FAT32 test: no block device");
        return;
    }

    // --- find the partition ---------------------------------------------------
    af_partition_table_t table;
    af_status_t rc = partition_table_read(dev, &table);
    if (af_status_err(rc)) {
        af_panic("fat32 self test: could not read the partition table (%s)",
                 af_status_name(rc));
    }

    partition_dump(&table);

    af_partition_t esp;
    rc = partition_find_esp(&table, &esp);
    if (af_status_err(rc)) {
        af_panic("fat32 self test: no EFI System Partition on %s", dev->name);
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   found the EFI System Partition at "
                                  "LBA %llu (%llu MiB)",
           (unsigned long long)esp.start_lba,
           (unsigned long long)(esp.sector_count * SECTOR_SIZE / AF_MIB));

    // --- mount ----------------------------------------------------------------
    static fat32_volume_t volume;

    rc = fat32_mount(&volume, dev, &esp);
    if (af_status_err(rc)) {
        af_panic("fat32 self test: mount failed (%s)", af_status_name(rc));
    }

    fat32_dump_info(&volume);

    af_log(AF_LOG_DEBUG, "test", "  ok   mounted FAT32 from the ESP");

    // --- list the root directory ---------------------------------------------
    af_u32 entries = 0;
    for (af_u32 i = 0; i < 64; i++) {
        fat32_entry_t e;
        rc = fat32_readdir(&volume, volume.root_cluster, i, &e);
        if (rc == AF_ERR_NOENT) {
            break;
        }
        if (af_status_err(rc)) {
            break;
        }
        af_info("test", "  /%s%s  (%u bytes)",
                e.name, e.is_directory ? "/" : "", e.size);
        entries++;
    }

    if (entries == 0) {
        af_panic("fat32 self test: the root directory is empty, which cannot be "
                 "right for the disk this system booted from");
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   listed %u entries in the root directory",
           entries);

    // --- read the acceptance file --------------------------------------------
    static char buffer[4096];
    af_u32 bytes_read = 0;

    rc = fat32_read_file(&volume, "/HELLO.TXT", buffer, sizeof(buffer) - 1,
                         &bytes_read);
    if (af_status_err(rc)) {
        af_panic("fat32 self test: reading /HELLO.TXT failed (%s). The file is "
                 "written into the ESP by tools/mkimage.py; if the image was "
                 "built without it, rebuild it.", af_status_name(rc));
    }

    buffer[bytes_read] = '\0';

    // The expected contents, checked exactly. Trailing newline included, so a
    // reader that returns the right bytes but the wrong length fails too.
    static const char expected[] = "Hello from disk\n";

    if (bytes_read != sizeof(expected) - 1 ||
        af_memcmp(buffer, expected, sizeof(expected) - 1) != 0) {
        af_panic("fat32 self test: /HELLO.TXT contains %u bytes; expected %u "
                 "matching \"Hello from disk\\n\". Got: \"%s\"",
                 bytes_read, (af_u32)(sizeof(expected) - 1), buffer);
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   read %u bytes from /HELLO.TXT and the contents match exactly",
           bytes_read);

    af_info("test", "  file contents: \"%s\"", buffer);

    // --- path resolution ------------------------------------------------------
    af_status_t missing = fat32_lookup(&volume, "/NO_SUCH_FILE.TXT", NULL);
    if (missing != AF_ERR_INVAL && missing != AF_ERR_NOENT) {
        af_panic("fat32 self test: looking up a missing path returned %s",
                 af_status_name(missing));
    }

    fat32_entry_t entry;
    missing = fat32_lookup(&volume, "/THIS_DOES_NOT_EXIST", &entry);
    if (missing != AF_ERR_NOENT) {
        af_panic("fat32 self test: looking up a missing path returned %s, "
                 "expected ERR_NOENT", af_status_name(missing));
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   a missing path returns ERR_NOENT rather than walking off "
           "the directory");

    af_marker("AF_FS_OK");
}

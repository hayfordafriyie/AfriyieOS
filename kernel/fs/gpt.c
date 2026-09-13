// SPDX-License-Identifier: MIT
// AfriyieOS — partition table parsing (GPT)

#include "afriyie/fs.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

// The EFI System Partition type GUID, in GPT's mixed-endian on-disk encoding:
// C12A7328-F81F-11D2-BA4B-00A0C93EC93B
//
// The first three fields are little-endian and the last two are big-endian. This
// is not a detail that can be guessed at: a GUID assembled in the wrong byte
// order simply never matches, and the failure looks exactly like "the disk has no
// ESP" rather than "the comparison is wrong".
static const af_u8 k_esp_type_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
};

#define SECTOR_SIZE 512

static af_u32 read_le32(const af_u8 *p)
{
    return (af_u32)p[0] | ((af_u32)p[1] << 8) |
           ((af_u32)p[2] << 16) | ((af_u32)p[3] << 24);
}

static af_u64 read_le64(const af_u8 *p)
{
    return (af_u64)read_le32(p) | ((af_u64)read_le32(p + 4) << 32);
}

af_status_t partition_table_read(af_block_device_t *dev,
                                 af_partition_table_t *out)
{
    if (dev == NULL || out == NULL) {
        return AF_ERR_INVAL;
    }

    af_memset(out, 0, sizeof(*out));

    static af_u8 sector[SECTOR_SIZE];

    af_status_t rc = block_read(dev, 0, 1, sector);
    if (af_status_err(rc)) {
        return rc;
    }

    // The MBR boot signature. Its absence means this is not a partitioned disk
    // at all — a valid outcome, but one the caller needs to be told about
    // explicitly rather than being handed an empty table.
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        af_warn("gpt", "%s: sector 0 has no 0x55AA signature — not a "
                       "partitioned disk", dev->name);
        return AF_ERR_NOENT;
    }

    // --- GPT ----------------------------------------------------------------
    static af_u8 header[SECTOR_SIZE];

    rc = block_read(dev, 1, 1, header);
    if (af_status_err(rc)) {
        return rc;
    }

    if (af_memcmp(header, "EFI PART", 8) != 0) {
        // A plain MBR disk. Reported as such: the caller may still find what it
        // needs in the protective MBR, but nothing here parses MBR partitions
        // because nothing in the boot path produces them.
        af_info("gpt", "%s: sector 1 is not a GPT header — MBR-only disk",
                dev->name);
        return AF_ERR_NOTSUP;
    }

    out->is_gpt = true;

    af_u64 entries_lba = read_le64(header + 72);
    af_u32 entry_count = read_le32(header + 80);
    af_u32 entry_size  = read_le32(header + 84);

    if (entry_size < 128 || entry_size > 512) {
        af_error("gpt", "implausible partition entry size %u", entry_size);
        return AF_ERR_FS_CORRUPT;
    }

    if (entry_count > 128) {
        entry_count = 128;
    }

    // Read the entries one at a time. A whole entry array is 16 KiB — four
    // sectors — which would need either a bigger static buffer or an allocation
    // in a code path that runs before anything is guaranteed to exist. One
    // sector at a time needs neither.
    af_u32 entries_per_sector = SECTOR_SIZE / entry_size;
    if (entries_per_sector == 0) {
        entries_per_sector = 1;
    }

    static af_u8 entries[SECTOR_SIZE];

    for (af_u32 i = 0; i < entry_count && out->count < AF_MAX_PARTITIONS; i++) {
        af_u32 sector_index = i / entries_per_sector;
        af_u32 offset = (i % entries_per_sector) * entry_size;

        rc = block_read(dev, entries_lba + sector_index, 1, entries);
        if (af_status_err(rc)) {
            return rc;
        }

        const af_u8 *e = entries + offset;

        // An all-zero type GUID marks an unused entry.
        bool empty = true;
        for (af_u32 b = 0; b < 16; b++) {
            if (e[b] != 0) {
                empty = false;
                break;
            }
        }
        if (empty) {
            continue;
        }

        af_partition_t *p = &out->entries[out->count];

        af_memcpy(p->type_guid, e, 16);
        p->start_lba    = read_le64(e + 32);
        p->sector_count = read_le64(e + 40) - p->start_lba + 1;

        // The name is UTF-16LE in up to 36 code units. Only the ASCII range is
        // converted; anything else becomes '?', which is honest about what this
        // does and cannot produce mojibake in a log.
        const af_u8 *name = e + 56;
        af_u32 n = 0;
        for (af_u32 c = 0; c < 36 && n < sizeof(p->name) - 1; c++) {
            af_u16 unit = (af_u16)(name[c * 2] | (name[c * 2 + 1] << 8));
            if (unit == 0) {
                break;
            }
            p->name[n++] = (unit < 128) ? (char)unit : '?';
        }
        p->name[n] = '\0';

        out->count++;
    }

    return AF_OK;
}

af_status_t partition_find_esp(const af_partition_table_t *table,
                               af_partition_t *out)
{
    if (table == NULL || out == NULL) {
        return AF_ERR_INVAL;
    }

    for (af_u32 i = 0; i < table->count; i++) {
        if (af_memcmp(table->entries[i].type_guid, k_esp_type_guid, 16) == 0) {
            *out = table->entries[i];
            return AF_OK;
        }
    }

    return AF_ERR_NOENT;
}

void partition_dump(const af_partition_table_t *table)
{
    if (table == NULL || table->count == 0) {
        af_info("gpt", "no partitions found");
        return;
    }

    af_info("gpt", "%s with %u partition(s):%s",
            table->is_gpt ? "GPT" : "MBR", table->count,
            table->is_gpt ? "" : " (no GPT header)");

    for (af_u32 i = 0; i < table->count; i++) {
        const af_partition_t *p = &table->entries[i];
        af_info("gpt", "  [%u] LBA %llu..%llu (%llu MiB)  '%s'",
                i, (unsigned long long)p->start_lba,
                (unsigned long long)(p->start_lba + p->sector_count - 1),
                (unsigned long long)(p->sector_count * SECTOR_SIZE / AF_MIB),
                p->name);
    }
}

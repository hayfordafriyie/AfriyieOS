// SPDX-License-Identifier: MIT
// AfriyieOS — block device abstraction

#ifndef AFRIYIE_BLOCK_H
#define AFRIYIE_BLOCK_H

#include "types.h"
#include "status.h"

#define AF_BLOCK_SECTOR_SIZE 512

// A block device is anything that can read and write fixed-size sectors. At
// v0.3 there is exactly one implementation (virtio-blk) and the abstraction is
// thin on purpose — the point is that the file system service above it never
// names a driver.
typedef struct af_block_device {
    const char *name;
    af_u64      sector_count;
    af_u32      sector_size;

    // Read `count` sectors starting at `lba` into `buffer`.
    // The buffer must be at least count * sector_size bytes.
    af_status_t (*read)(struct af_block_device *dev, af_u64 lba,
                        af_u32 count, void *buffer);

    af_status_t (*write)(struct af_block_device *dev, af_u64 lba,
                         af_u32 count, const void *buffer);

    bool        read_only;
    void       *driver_data;
} af_block_device_t;

// The device the system booted from, once a driver has claimed it.
af_block_device_t *block_boot_device(void);
void block_set_boot_device(af_block_device_t *dev);

// Convenience wrappers with bounds checking, so a caller cannot ask a device
// for sectors it does not have. Every out-of-range request is a bug somewhere,
// and catching it here names the caller rather than producing a device error
// several layers down.
af_status_t block_read(af_block_device_t *dev, af_u64 lba, af_u32 count,
                       void *buffer);
af_status_t block_write(af_block_device_t *dev, af_u64 lba, af_u32 count,
                        const void *buffer);

#endif // AFRIYIE_BLOCK_H

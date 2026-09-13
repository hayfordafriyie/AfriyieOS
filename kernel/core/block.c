// SPDX-License-Identifier: MIT
// AfriyieOS — block device layer

#include "afriyie/block.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"

static af_block_device_t *s_boot_device = NULL;

af_block_device_t *block_boot_device(void)
{
    return s_boot_device;
}

void block_set_boot_device(af_block_device_t *dev)
{
    s_boot_device = dev;
}

// -----------------------------------------------------------------------------
// Bounds-checked wrappers
//
// The check lives here rather than in each driver because an out-of-range
// request is a bug in a caller, and a device error reported from three layers
// down names the wrong suspect. A driver that receives a bad LBA will do
// whatever the hardware does with it, which is usually to read the wrong sector
// silently.
// -----------------------------------------------------------------------------
static af_status_t check_range(af_block_device_t *dev, af_u64 lba,
                               af_u32 count)
{
    if (dev == NULL) {
        return AF_ERR_NODEV;
    }
    if (count == 0) {
        return AF_ERR_INVAL;
    }

    // An overflow here wraps the end check and lets a large LBA through.
    if (lba >= dev->sector_count) {
        af_error("block", "%s: LBA %llu is past the end of the device "
                          "(%llu sectors)",
                 dev->name, (unsigned long long)lba,
                 (unsigned long long)dev->sector_count);
        return AF_ERR_INVAL;
    }

    if (lba + count > dev->sector_count) {
        af_error("block", "%s: read of %u sectors from LBA %llu runs past the "
                          "end of the device (%llu sectors)",
                 dev->name, count, (unsigned long long)lba,
                 (unsigned long long)dev->sector_count);
        return AF_ERR_INVAL;
    }

    return AF_OK;
}

af_status_t block_read(af_block_device_t *dev, af_u64 lba, af_u32 count,
                       void *buffer)
{
    af_status_t rc = check_range(dev, lba, count);
    if (af_status_err(rc)) {
        return rc;
    }

    if (dev->read == NULL) {
        return AF_ERR_NOTSUP;
    }

    return dev->read(dev, lba, count, buffer);
}

af_status_t block_write(af_block_device_t *dev, af_u64 lba, af_u32 count,
                        const void *buffer)
{
    af_status_t rc = check_range(dev, lba, count);
    if (af_status_err(rc)) {
        return rc;
    }

    if (dev->write == NULL || dev->read_only) {
        return AF_ERR_ROFS;
    }

    return dev->write(dev, lba, count, buffer);
}

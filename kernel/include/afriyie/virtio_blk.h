// SPDX-License-Identifier: MIT
// AfriyieOS — virtio-blk driver interface
//
// Kernel-mode at v0.3. From v0.7 this becomes a user-space driver process
// behind the driver SDK, and only the initialisation entry point here changes —
// the block device interface it presents to the file system does not.

#ifndef AFRIYIE_VIRTIO_BLK_H
#define AFRIYIE_VIRTIO_BLK_H

#include "types.h"
#include "status.h"

// Finds the device, negotiates features, sets up the virtqueue and registers
// the block device. Returns AF_ERR_NODEV when no device is present, which is
// not a fatal condition: a machine with no virtio disk still boots and runs.
af_status_t virtio_blk_init(void);

bool virtio_blk_ready(void);

// Reads sector 0 and checks the boot signature, then prints a hexdump of the
// partition table. This is the v0.3 acceptance evidence: the first time the
// kernel reads something from hardware it does not own.
void virtio_blk_selftest(void);

#endif // AFRIYIE_VIRTIO_BLK_H

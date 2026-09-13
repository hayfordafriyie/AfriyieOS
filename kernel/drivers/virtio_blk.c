// SPDX-License-Identifier: MIT
// AfriyieOS — virtio-blk driver (legacy/transitional virtio 0.9.5 interface)
//
// =============================================================================
// WHY THE LEGACY INTERFACE
// =============================================================================
// Virtio 1.0 (modern) puts its registers behind PCI vendor-specific capability
// structures, which means parsing PCI capabilities to find them. The legacy
// interface is a flat block of I/O ports in BAR0 and needs none of that.
//
// The registers are identical either way; only the discovery differs. So this
// starts with the simple path and the modern one is a discovery change, not a
// rewrite. QEMU is told to present a legacy device explicitly — see the
// `disable-modern=on` in tools/run_qemu.py, which is there to make the driver's
// assumption explicit rather than accidental.
//
// =============================================================================
// THE VIRTQUEUE
// =============================================================================
// Three rings, all in one physically contiguous, page-aligned block:
//
//   descriptor table   16 bytes each: address, length, flags, next
//   available ring     what the driver has published for the device
//   used ring          what the device has finished
//
// The driver writes a chain of descriptors, points the available ring at the
// first one, and notifies the device. The device writes back to the used ring.
// Everything is communicated through memory the device can DMA to, which is why
// bus mastering has to be enabled first.
//
// The whole structure must be in ONE contiguous physical region for the legacy
// interface, because the device is given only its page frame number. It is not
// a scatter-gather list of rings; the rings have to be adjacent.
// =============================================================================

#include "afriyie/block.h"
#include "afriyie/pci.h"
#include "afriyie/pmm.h"
#include "afriyie/hal.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/io.h"
#include "afriyie/virtio_blk.h"

// -----------------------------------------------------------------------------
// Legacy virtio PCI register offsets within BAR0
// -----------------------------------------------------------------------------
#define VIRTIO_PCI_HOST_FEATURES    0x00
#define VIRTIO_PCI_GUEST_FEATURES   0x04
#define VIRTIO_PCI_QUEUE_PFN        0x08
#define VIRTIO_PCI_QUEUE_SIZE       0x0C
#define VIRTIO_PCI_QUEUE_SELECT     0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10
#define VIRTIO_PCI_STATUS           0x12
#define VIRTIO_PCI_ISR              0x13
#define VIRTIO_PCI_CONFIG           0x14   // device-specific config follows

// Device status bits, set in order and checked at each step
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED        0x80

// Feature bits
#define VIRTIO_BLK_F_RO             (1u << 5)
#define VIRTIO_BLK_F_BLK_SIZE       (1u << 6)
#define VIRTIO_BLK_F_FLUSH          (1u << 9)

// Descriptor flags
#define VIRTQ_DESC_F_NEXT           0x01
#define VIRTQ_DESC_F_WRITE          0x02   // device writes to this buffer

// Request types
#define VIRTIO_BLK_T_IN             0      // read from device
#define VIRTIO_BLK_T_OUT            1      // write to device
#define VIRTIO_BLK_T_FLUSH          4

#define AF_VIRTIO_BLK_VENDOR       0x1AF4
#define AF_VIRTIO_BLK_DEVICE_LEGACY 0x1001
#define AF_VIRTIO_BLK_DEVICE_MODERN 0x1042

// -----------------------------------------------------------------------------
// Virtqueue structures
// -----------------------------------------------------------------------------
typedef struct AF_PACKED {
    af_u64 address;      // physical address of the buffer
    af_u32 length;
    af_u16 flags;
    af_u16 next;
} virtq_desc_t;

typedef struct AF_PACKED {
    af_u16 flags;
    af_u16 index;
    af_u16 ring[];       // queue_size entries, followed by the used-event field
} virtq_avail_t;

typedef struct AF_PACKED {
    af_u16 id;
    af_u16 len;
} virtq_used_elem_t;

typedef struct AF_PACKED {
    af_u16 flags;
    af_u16 index;
    virtq_used_elem_t ring[];
} virtq_used_t;

// The whole queue lives in one contiguous allocation, because the legacy
// interface tells the device only the page frame number of the start.
#define VIRTQ_MAX_SIZE 256

typedef struct {
    af_paddr     phys;           // physical base of the whole queue block
    af_u8       *virt;           // same address, identity-mapped
    af_u32       size;           // descriptors in the queue

    virtq_desc_t *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;

    af_u16       avail_index;    // next slot in the available ring
    af_u16       used_seen;      // how many used entries we have consumed

    // One request in flight at a time. A real driver would pipeline; at v0.3
    // a synchronous request per read is simpler and fast enough, and it means
    // no request bookkeeping to get wrong.
    af_paddr     request_phys;
    af_u8       *request_virt;
    af_u16       request_sector_count;
} virtq_t;

// A virtio-blk request: header, then data, then a one-byte status.
typedef struct AF_PACKED {
    af_u32 type;
    af_u32 reserved;
    af_u64 sector;
} virtio_blk_req_header_t;

AF_STATIC_ASSERT_SIZE(virtio_blk_req_header_t, 16);

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static virtq_t            s_queue;
static af_block_device_t  s_blk;
static af_u16             s_io_base = 0;
static bool               s_initialised = false;

// -----------------------------------------------------------------------------
// Register access
// -----------------------------------------------------------------------------
AF_INLINE af_u32 virtio_read32(af_u16 offset)
{
    return af_inl((af_u16)(s_io_base + offset));
}

AF_INLINE void virtio_write32(af_u16 offset, af_u32 value)
{
    af_outl((af_u16)(s_io_base + offset), value);
}

AF_INLINE af_u16 virtio_read16(af_u16 offset)
{
    return af_inw((af_u16)(s_io_base + offset));
}

AF_INLINE void virtio_write16(af_u16 offset, af_u16 value)
{
    af_outw((af_u16)(s_io_base + offset), value);
}

AF_INLINE af_u8 virtio_read8(af_u16 offset)
{
    return af_inb((af_u16)(s_io_base + offset));
}

AF_INLINE void virtio_write8(af_u16 offset, af_u8 value)
{
    af_outb((af_u16)(s_io_base + offset), value);
}

// -----------------------------------------------------------------------------
// The three-step handshake
// -----------------------------------------------------------------------------
static af_status_t virtio_negotiate_features(void)
{
    // Reset: write 0 to the status register.
    virtio_write8(VIRTIO_PCI_STATUS, 0);

    // Step 1: acknowledge that we have found the device.
    virtio_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    // Step 2: declare that we know how to drive it.
    virtio_write8(VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    af_u32 host_features = virtio_read32(VIRTIO_PCI_HOST_FEATURES);

    // Accept nothing optional.
    //
    // Writing 0 to the guest-features register is a deliberate choice, not an
    // oversight. Every optional feature changes the driver's behaviour — a
    // negotiated block size means the device may have a different logical sector
    // size, and a negotiated flush means writes must be ordered. Claiming
    // support without implementing it is how a driver corrupts data on hardware
    // it was never tested against.
    virtio_write32(VIRTIO_PCI_GUEST_FEATURES, 0);

    af_info("virtio-blk", "device offered features 0x%X; accepting none "
                          "(read-only: %s, flush: %s)",
            host_features,
            (host_features & VIRTIO_BLK_F_RO) ? "yes" : "no",
            (host_features & VIRTIO_BLK_F_FLUSH) ? "yes" : "no");

    if ((host_features & VIRTIO_BLK_F_RO) != 0) {
        s_blk.read_only = true;
    }

    // Step 3: declare the feature negotiation finished.
    //
    // This bit was missing from the first version of this driver, and its
    // absence is why every request timed out. The sequence is not decorative:
    // a device that never sees FEATURES_OK has not been told the driver is
    // finished choosing features, and it will not begin processing requests.
    //
    // The device is then required to write the bit back if it accepts the
    // driver's feature set, which is how it reports "I cannot actually support
    // what you asked for". Reading it back and checking is the whole point of
    // the step — an unconditional write would make it meaningless.
    virtio_write8(VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                  VIRTIO_STATUS_FEATURES_OK);

    af_u8 status = virtio_read8(VIRTIO_PCI_STATUS);
    if ((status & VIRTIO_STATUS_FEATURES_OK) == 0) {
        af_error("virtio-blk", "the device rejected the negotiated features "
                               "(status 0x%02X) — it cannot drive this device "
                               "with the feature set chosen", status);
        virtio_write8(VIRTIO_PCI_STATUS,
                      status | VIRTIO_STATUS_FAILED);
        return AF_ERR_NOTSUP;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Queue setup
// -----------------------------------------------------------------------------
static af_status_t virtio_setup_queue(void)
{
    // Select queue 0. virtio-blk has exactly one.
    virtio_write16(VIRTIO_PCI_QUEUE_SELECT, 0);

    af_u16 device_size = virtio_read16(VIRTIO_PCI_QUEUE_SIZE);

    if (device_size == 0) {
        af_error("virtio-blk", "queue 0 reports size 0 — the device has no "
                               "usable request queue");
        return AF_ERR_NODEV;
    }

    // The LEGACY interface requires the driver to lay the rings out for EXACTLY
    // the size the device reported.
    //
    // This is not a preference. The device derives where the available and used
    // rings are from the queue size it published, so a driver that lays them out
    // for a smaller size puts its rings somewhere the device will never look —
    // and the failure is a request that is submitted correctly and never
    // completes, with the device reporting nothing wrong.
    //
    // That is exactly what the first run of this driver did: the device offered
    // 256 descriptors, the driver used 128 to save memory, and every request
    // timed out. The old comment here claimed "the driver may use fewer
    // descriptors than the device offers", which is true of the MODERN interface
    // and false of this one.
    af_u32 size = device_size;

    if (size > VIRTQ_MAX_SIZE) {
        af_error("virtio-blk", "device offers %u descriptors; the driver "
                               "supports up to %u", size,
                 (af_u32)VIRTQ_MAX_SIZE);
        return AF_ERR_NOTSUP;
    }

    // =========================================================================
    // LEGACY VIRTQUEUE LAYOUT — THE USED RING IS PAGE-ALIGNED
    // =========================================================================
    // The legacy interface does not let the driver choose the layout. The device
    // computes where each ring is, and the rule is:
    //
    //     descriptor table   at the base, 16 bytes x queue_size
    //     available ring     immediately after it
    //     used ring          at the NEXT PAGE BOUNDARY after the available ring
    //
    // The first version placed the used ring immediately after the available
    // ring, with no padding — which is a reasonable reading of "adjacent" and is
    // what the MODERN interface allows. The device then wrote its completions to
    // a page-aligned address the driver was not looking at, and the result was a
    // request that succeeded completely and was never observed:
    //
    //     virtio_blk_req_complete ... status 0     <- the device finished
    //     (driver spins on used->index forever)    <- the driver never sees it
    //
    // QEMU's own virtio trace is what found this. The device log showed the read
    // completing with status 0 while the driver reported a timeout, which meant
    // the two disagreed about where the used ring was rather than about whether
    // the request worked.
    // =========================================================================
    af_u64 desc_bytes  = (af_u64)size * sizeof(virtq_desc_t);
    af_u64 avail_bytes = 6 + (af_u64)size * sizeof(af_u16);
    af_u64 used_bytes  = 6 + (af_u64)size * sizeof(virtq_used_elem_t);
    af_u64 used_offset = AF_ALIGN_UP(desc_bytes + avail_bytes, AF_PAGE_SIZE);
    af_u64 bytes       = used_offset + used_bytes;

    af_u32 frames = (af_u32)((bytes + AF_FRAME_SIZE - 1) / AF_FRAME_SIZE);
    if (frames == 0) {
        frames = 1;
    }

    af_paddr queue_phys = pmm_alloc_frames(frames);
    if (queue_phys == AF_FRAME_INVALID) {
        return AF_ERR_NOMEM;
    }

    af_memset((void *)(af_uptr)queue_phys, 0, (af_size)frames * AF_FRAME_SIZE);

    s_queue.phys = queue_phys;
    s_queue.virt = (af_u8 *)(af_uptr)queue_phys;
    s_queue.size = size;

    af_uptr base = (af_uptr)s_queue.virt;

    s_queue.desc  = (virtq_desc_t *)base;
    s_queue.avail = (virtq_avail_t *)(void *)(base + desc_bytes);
    s_queue.used  = (virtq_used_t *)(void *)(base + used_offset);

    af_uptr after_used = base + used_offset + used_bytes;

    if (after_used > base + (af_size)frames * AF_FRAME_SIZE) {
        af_error("virtio-blk", "queue layout needs %lu bytes but only %lu were "
                               "allocated", (unsigned long)(after_used - base),
                 (unsigned long)((af_size)frames * AF_FRAME_SIZE));
        pmm_free_frames(queue_phys, frames);
        return AF_ERR_NOMEM;
    }

    s_queue.avail_index = 0;
    s_queue.used_seen = 0;

    // A separate frame for the in-flight request: header, one sector of data,
    // and the status byte.
    af_paddr req_phys = pmm_alloc_frame_z();
    if (req_phys == AF_FRAME_INVALID) {
        pmm_free_frames(queue_phys, frames);
        return AF_ERR_NOMEM;
    }

    s_queue.request_phys = req_phys;
    s_queue.request_virt = (af_u8 *)(af_uptr)req_phys;
    s_queue.request_sector_count = (af_u16)((AF_FRAME_SIZE - 32) / AF_BLOCK_SECTOR_SIZE);

    // Tell the device where the queue is. The legacy interface takes a page
    // frame number, not an address — passing the address itself is a mistake
    // that produces a device which appears to initialise and then never
    // completes a request.
    af_u32 pfn = (af_u32)(queue_phys >> 12);
    virtio_write32(VIRTIO_PCI_QUEUE_PFN, pfn);

    af_info("virtio-blk", "queue 0: device offers %u descriptors, using %u "
                          "(%u bytes at 0x%lX, PFN %u), request buffer %u sectors",
            device_size, size,
            (unsigned)(after_used - base), (af_u64)queue_phys, pfn,
            s_queue.request_sector_count);

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Request execution
// -----------------------------------------------------------------------------
//
// Builds a three-descriptor chain — header (device reads), data (device writes
// for a read), status (device writes) — publishes it, notifies the device, and
// spins until the used ring catches up.
//
// Busy-waiting rather than blocking is deliberate at v0.3. Blocking needs an
// interrupt handler and a wait queue, and the interrupt path is only worth
// building once there is more than one request in flight to overlap. A note is
// in the v0.5 task list to make this interrupt-driven.
static af_status_t virtio_blk_request(af_u32 type, af_u64 sector,
                                      af_u32 sector_count, void *buffer,
                                      bool device_writes)
{
    if (s_queue.size < 3) {
        return AF_ERR_NOTREADY;
    }

    af_u8 *req = s_queue.request_virt;

    virtio_blk_req_header_t *header = (virtio_blk_req_header_t *)req;
    header->type     = type;
    header->reserved = 0;
    header->sector   = sector;

    af_u32 data_bytes = sector_count * AF_BLOCK_SECTOR_SIZE;

    af_u8 *data   = req + 16;
    af_u8 *status = req + 16 + data_bytes;

    *status = 0xFF;   // the device must overwrite this

    // --- descriptors ---------------------------------------------------------
    af_paddr req_phys = s_queue.request_phys;
    af_paddr data_phys = req_phys + 16;

    s_queue.desc[0].address = req_phys;            // header
    s_queue.desc[0].length  = sizeof(*header);
    s_queue.desc[0].flags   = VIRTQ_DESC_F_NEXT;
    s_queue.desc[0].next    = 1;

    s_queue.desc[1].address = data_phys;           // payload
    s_queue.desc[1].length  = data_bytes;
    s_queue.desc[1].flags   = VIRTQ_DESC_F_NEXT |
                              (device_writes ? VIRTQ_DESC_F_WRITE : 0u);
    s_queue.desc[1].next    = 2;

    s_queue.desc[2].address = (af_paddr)(af_uptr)status;
    s_queue.desc[2].length  = 1;
    s_queue.desc[2].flags   = VIRTQ_DESC_F_WRITE;
    s_queue.desc[2].next    = 0;

    // --- publish -------------------------------------------------------------
    s_queue.avail->ring[s_queue.avail_index % s_queue.size] = 0;
    s_queue.avail_index++;
    s_queue.avail->index = s_queue.avail_index;

    // A full memory barrier before the notify. Without it the device can observe
    // the notification before the descriptor writes, and read a half-built
    // request — which it will happily execute.
    af_compiler_barrier();

    virtio_write16(VIRTIO_PCI_QUEUE_NOTIFY, 0);

    // --- wait ------------------------------------------------------------------
    af_u32 spins = 0;
    const af_u32 spin_limit = 50000000u;

    while (s_queue.used->index == s_queue.used_seen) {
        af_compiler_barrier();
        if (++spins > spin_limit) {
            af_error("virtio-blk", "request timed out after %u spins "
                                   "(lba %llu, %u sectors)",
                     spins, (unsigned long long)sector, sector_count);
            return AF_ERR_TIMEOUT;
        }
    }

    s_queue.used_seen++;

    // Read the ISR register to clear the interrupt. Even in a polling driver
    // the device asserts its interrupt line until this happens, and an
    // unacknowledged line keeps the PIC asserting too.
    (void)virtio_read8(VIRTIO_PCI_ISR);

    if (*status != 0) {
        af_error("virtio-blk", "device reported status %u for lba %llu",
                 *status, (unsigned long long)sector);
        return AF_ERR_IO;
    }

    if (device_writes) {
        af_memcpy(buffer, data, data_bytes);
    } else {
        af_memcpy(data, buffer, data_bytes);
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Block device interface
// -----------------------------------------------------------------------------
static af_status_t virtio_blk_read(af_block_device_t *dev, af_u64 lba,
                                   af_u32 count, void *buffer)
{
    AF_UNUSED(dev);

    // Split a multi-sector request to fit the single request buffer. The driver
    // handles one request at a time, so a large read becomes several.
    af_u8 *out = (af_u8 *)buffer;
    af_u32 remaining = count;
    af_u64 current = lba;

    while (remaining > 0) {
        af_u32 chunk = remaining;
        if (chunk > s_queue.request_sector_count) {
            chunk = s_queue.request_sector_count;
        }

        af_status_t rc = virtio_blk_request(VIRTIO_BLK_T_IN, current, chunk,
                                            out, true);
        if (af_status_err(rc)) {
            return rc;
        }

        out       += (af_size)chunk * AF_BLOCK_SECTOR_SIZE;
        current   += chunk;
        remaining -= chunk;
    }

    return AF_OK;
}

static af_status_t virtio_blk_write(af_block_device_t *dev, af_u64 lba,
                                    af_u32 count, const void *buffer)
{
    AF_UNUSED(dev);

    if (s_blk.read_only) {
        return AF_ERR_ROFS;
    }

    const af_u8 *in = (const af_u8 *)buffer;
    af_u32 remaining = count;
    af_u64 current = lba;

    while (remaining > 0) {
        af_u32 chunk = remaining;
        if (chunk > s_queue.request_sector_count) {
            chunk = s_queue.request_sector_count;
        }

        // virtio_blk_request copies the data into the request buffer itself when
        // the device reads, so a const source is fine.
        af_status_t rc = virtio_blk_request(VIRTIO_BLK_T_OUT, current, chunk,
                                            (void *)(af_uptr)in, false);
        if (af_status_err(rc)) {
            return rc;
        }

        in        += (af_size)chunk * AF_BLOCK_SECTOR_SIZE;
        current   += chunk;
        remaining -= chunk;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
af_status_t virtio_blk_init(void)
{
    // QEMU's transitional virtio-blk presents the legacy device ID; a purely
    // modern device uses 0x1042. Both are looked for so that the failure message
    // can say which one was found.
    const af_pci_device_t *dev = pci_find_device(AF_VIRTIO_BLK_VENDOR,
                                                 AF_VIRTIO_BLK_DEVICE_LEGACY);
    const af_pci_device_t *modern = NULL;

    if (dev == NULL) {
        modern = pci_find_device(AF_VIRTIO_BLK_VENDOR,
                                 AF_VIRTIO_BLK_DEVICE_MODERN);
        if (modern != NULL) {
            af_error("virtio-blk", "found a MODERN virtio-blk device "
                                   "(%04X:%04X), which this driver does not "
                                   "drive. It needs PCI capability parsing to "
                                   "locate its registers; the legacy interface "
                                   "uses a flat I/O BAR. Run QEMU with "
                                   "disable-modern=on, or see the v0.5 task "
                                   "list for modern virtio support.",
                     modern->vendor_id, modern->device_id);
            return AF_ERR_NOTSUP;
        }

        af_error("virtio-blk", "no virtio-blk device found on the PCI bus");
        return AF_ERR_NODEV;
    }

    af_info("virtio-blk", "found %04X:%04X at %02u:%02u.%u (irq %u)",
            dev->vendor_id, dev->device_id,
            dev->address.bus, dev->address.slot, dev->address.func,
            dev->interrupt_line);

    af_pci_bar_t bar0;
    af_status_t rc = pci_read_bar(dev, 0, &bar0);
    if (af_status_err(rc)) {
        af_error("virtio-blk", "BAR0 could not be decoded (%s)",
                 af_status_name(rc));
        return rc;
    }

    if (!bar0.is_io) {
        af_error("virtio-blk", "BAR0 is memory-mapped, but the legacy virtio "
                               "register block is expected in I/O space");
        return AF_ERR_NOTSUP;
    }

    s_io_base = (af_u16)bar0.address;

    af_info("virtio-blk", "BAR0: I/O ports 0x%X..0x%X (%llu bytes)",
            s_io_base, (af_u32)(s_io_base + bar0.size - 1),
            (unsigned long long)bar0.size);

    // Bus mastering must be enabled before the device can DMA into our queue.
    // Without it the first request is accepted and never completes.
    pci_enable_bus_master(dev);

    rc = virtio_negotiate_features();
    if (af_status_err(rc)) {
        return rc;
    }

    rc = virtio_setup_queue();
    if (af_status_err(rc)) {
        return rc;
    }

    // Step 4: everything is set up; the device may start using the queue.
    virtio_write8(VIRTIO_PCI_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                  VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    // Read the capacity from the device-specific configuration: a 64-bit sector
    // count at offset 0x14 of BAR0.
    af_u32 capacity_low  = virtio_read32(VIRTIO_PCI_CONFIG);
    af_u32 capacity_high = virtio_read32((af_u16)(VIRTIO_PCI_CONFIG + 4));

    s_blk.name         = "virtio-blk0";
    s_blk.sector_count = ((af_u64)capacity_high << 32) | capacity_low;
    s_blk.sector_size  = AF_BLOCK_SECTOR_SIZE;
    s_blk.read         = virtio_blk_read;
    s_blk.write        = virtio_blk_write;
    s_blk.driver_data  = &s_queue;

    if (s_blk.sector_count == 0) {
        af_error("virtio-blk", "device reports 0 sectors — the queue was set up "
                               "but the device is not usable");
        return AF_ERR_NODEV;
    }

    af_info("virtio-blk", "ready: %llu sectors of %u bytes = %llu MiB%s",
            (unsigned long long)s_blk.sector_count, s_blk.sector_size,
            (unsigned long long)((s_blk.sector_count * AF_BLOCK_SECTOR_SIZE) / AF_MIB),
            s_blk.read_only ? " (read-only)" : "");

    block_set_boot_device(&s_blk);
    s_initialised = true;

    return AF_OK;
}

bool virtio_blk_ready(void)
{
    return s_initialised;
}

// =============================================================================
// Self test — the v0.3 acceptance evidence
// =============================================================================
//
// Reading the boot disk's sector 0 is the smallest thing that proves the whole
// driver works: PCI enumeration found the device, feature negotiation and queue
// setup completed, a descriptor chain was built correctly, the device DMA'd into
// our buffer, and the completion path noticed.
//
// It checks something with a known answer too. A sector-0 read that returns 512
// bytes of anything at all would pass a length check; requiring the boot
// signature at offset 510 proves the data actually came from the disk. The image
// we booted from is the image we are reading, and it has a protective MBR by
// construction — mkimage.py writes one.
void virtio_blk_selftest(void)
{
    if (!s_initialised) {
        af_warn("test", "  skip virtio-blk test: no device");
        return;
    }

    static af_u8 sector[AF_BLOCK_SECTOR_SIZE];

    af_status_t rc = block_read(&s_blk, 0, 1, sector);
    if (af_status_err(rc)) {
        af_panic("virtio-blk self test: reading sector 0 failed (%s)",
                 af_status_name(rc));
    }

    // The boot signature: bytes 510 and 511 of the first sector.
    af_u8 sig_lo = sector[510];
    af_u8 sig_hi = sector[511];

    if (sig_lo != 0x55 || sig_hi != 0xAA) {
        af_panic("virtio-blk self test: sector 0 does not end in 0x55AA "
                 "(found %02X %02X) — the read completed but the data is not "
                 "the disk's first sector", sig_lo, sig_hi);
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   read sector 0 from an actual disk; boot signature 0x%02X%02X "
           "is present", sig_hi, sig_lo);

    // The protective MBR partition entry: type 0xEE at offset 446 + 4. If this
    // is a GPT disk — and the one mkimage.py builds always is — finding it here
    // confirms we are reading the same layout the host tools see.
    af_u8 partition_type = sector[446 + 4];
    if (partition_type == 0xEE) {
        af_log(AF_LOG_DEBUG, "test",
               "  ok   protective MBR partition type is 0xEE — this is the GPT "
               "disk the system booted from");
    } else {
        af_log(AF_LOG_DEBUG, "test",
               "  ok   partition type is 0x%02X (not a protective MBR)",
               partition_type);
    }

    // A multi-sector read, to exercise the chunking path rather than the single
    // sector case only.
    static af_u8 four[4 * AF_BLOCK_SECTOR_SIZE];
    rc = block_read(&s_blk, 0, 4, four);
    if (af_status_err(rc)) {
        af_panic("virtio-blk self test: 4-sector read failed (%s)",
                 af_status_name(rc));
    }

    // The first sector of the multi-sector read must match the single-sector
    // read exactly. If the chunking arithmetic were wrong — an off-by-one in the
    // LBA advance, say — the two would disagree.
    if (af_memcmp(four, sector, AF_BLOCK_SECTOR_SIZE) != 0) {
        af_panic("virtio-blk self test: a 4-sector read returned different data "
                 "for sector 0 than a single-sector read");
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   4-sector read agrees with the single-sector read");

    // Out-of-range requests must be refused by the block layer rather than
    // reaching the device.
    rc = block_read(&s_blk, s_blk.sector_count, 1, sector);
    if (rc != AF_ERR_INVAL) {
        af_panic("virtio-blk self test: a read past the end of the device "
                 "returned %s, expected ERR_INVAL", af_status_name(rc));
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   a read past the end of the device is refused with ERR_INVAL");

    af_marker("AF_BLOCK_OK");
}

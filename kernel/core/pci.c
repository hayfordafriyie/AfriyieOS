// SPDX-License-Identifier: MIT
// AfriyieOS — PCI bus enumeration via the legacy configuration mechanism
//
// =============================================================================
// THE 0xCF8/0xCFC MECHANISM
// =============================================================================
// A 32-bit address is written to port 0xCF8 and the configuration register is
// read or written at 0xCFC. It is slow — two port accesses per register — but it
// is universally available, needs no ACPI table to locate, and is what every
// driver's bring-up path uses first. Memory-mapped configuration (ECAM, from the
// ACPI MCFG table) is faster and arrives with the ACPI work at v0.5.
//
// The address word is laid out as:
//
//   bit 31    enable
//   bits 30:24  reserved
//   bits 23:16  bus
//   bits 15:11  device (slot)
//   bits 10:8   function
//   bits 7:2    register offset, in 32-bit words
//   bits 1:0    must be zero
//
// Getting the offset shifted by two instead of one is a classic error: the
// field is a WORD index, so a byte offset must be divided by four first. The
// symptom is reading a register sixteen bytes away from the one you asked for.
// =============================================================================

#include "afriyie/pci.h"
#include "afriyie/io.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_VENDOR_NONE    0xFFFF

// -----------------------------------------------------------------------------
// Configuration space access
// -----------------------------------------------------------------------------
static af_u32 config_address(af_pci_address_t addr, af_u8 offset)
{
    return (af_u32)((1u << 31) |
                    ((af_u32)addr.bus  << 16) |
                    ((af_u32)addr.slot << 11) |
                    ((af_u32)addr.func << 8)  |
                    ((af_u32)offset & 0xFC));   // word-aligned
}

af_u32 af_pci_read32(af_pci_address_t addr, af_u8 offset)
{
    af_outl(PCI_CONFIG_ADDRESS, config_address(addr, offset));
    return af_inl(PCI_CONFIG_DATA);
}

af_u16 af_pci_read16(af_pci_address_t addr, af_u8 offset)
{
    af_u32 value = af_pci_read32(addr, offset);
    return (af_u16)((value >> ((offset & 2) * 8)) & 0xFFFF);
}

af_u8 af_pci_read8(af_pci_address_t addr, af_u8 offset)
{
    af_u32 value = af_pci_read32(addr, offset);
    return (af_u8)((value >> ((offset & 3) * 8)) & 0xFF);
}

void af_pci_write32(af_pci_address_t addr, af_u8 offset, af_u32 value)
{
    af_outl(PCI_CONFIG_ADDRESS, config_address(addr, offset));
    af_outl(PCI_CONFIG_DATA, value);
}

void af_pci_write16(af_pci_address_t addr, af_u8 offset, af_u16 value)
{
    af_u32 old = af_pci_read32(addr, offset);
    af_u32 shift = (offset & 2) * 8;
    af_u32 mask = (af_u32)0xFFFF << shift;

    af_pci_write32(addr, offset, (old & ~mask) | ((af_u32)value << shift));
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static af_pci_device_t s_devices[AF_PCI_MAX_DEVICES];
static af_u32           s_device_count = 0;

af_u32 pci_device_count(void)
{
    return s_device_count;
}

const af_pci_device_t *pci_device(af_u32 index)
{
    if (index >= s_device_count) {
        return NULL;
    }
    return &s_devices[index];
}

// -----------------------------------------------------------------------------
// Enumeration
// -----------------------------------------------------------------------------
static void probe_function(af_u8 bus, af_u8 slot, af_u8 func)
{
    af_pci_address_t addr = { bus, slot, func, 0 };

    af_u32 id = af_pci_read32(addr, 0x00);
    af_u16 vendor = (af_u16)(id & 0xFFFF);

    // 0xFFFF means nothing is there. For function 0 this ends the slot; for
    // functions 1-7 it just means that function is absent, which is normal.
    if (vendor == PCI_VENDOR_NONE) {
        return;
    }

    if (s_device_count >= AF_PCI_MAX_DEVICES) {
        return;
    }

    af_pci_device_t *dev = &s_devices[s_device_count];

    af_memset(dev, 0, sizeof(*dev));
    dev->address   = addr;
    dev->vendor_id = vendor;
    dev->device_id = (af_u16)((id >> 16) & 0xFFFF);

    af_u32 class_reg = af_pci_read32(addr, 0x08);
    dev->revision  = (af_u8)(class_reg & 0xFF);
    dev->prog_if   = (af_u8)((class_reg >> 8) & 0xFF);
    dev->subclass  = (af_u8)((class_reg >> 16) & 0xFF);
    dev->class_code = (af_u8)((class_reg >> 24) & 0xFF);

    af_u32 header = af_pci_read32(addr, 0x0C);
    dev->header_type = (af_u8)((header >> 16) & 0xFF);

    dev->interrupt_line = af_pci_read8(addr, 0x3C);
    dev->interrupt_pin  = af_pci_read8(addr, 0x3D);

    for (af_u32 i = 0; i < 6; i++) {
        dev->bar[i] = af_pci_read32(addr, (af_u8)(0x10 + i * 4));
    }

    s_device_count++;
}

static bool is_multifunction(af_u8 bus, af_u8 slot)
{
    af_pci_address_t addr = { bus, slot, 0, 0 };
    af_u32 header = af_pci_read32(addr, 0x0C);
    return ((header >> 16) & 0x80) != 0;
}

static void probe_slot(af_u8 bus, af_u8 slot)
{
    af_pci_address_t addr = { bus, slot, 0, 0 };

    if ((af_pci_read32(addr, 0x00) & 0xFFFF) == PCI_VENDOR_NONE) {
        return;
    }

    probe_function(bus, slot, 0);

    // Only multi-function devices have functions 1-7. Probing them anyway is
    // mostly harmless but wastes time on a full bus scan, and on some chipsets
    // reading absent functions has side effects.
    if (is_multifunction(bus, slot)) {
        for (af_u8 func = 1; func < 8; func++) {
            probe_function(bus, slot, func);
        }
    }
}

af_status_t pci_init(void)
{
    s_device_count = 0;
    af_memset(s_devices, 0, sizeof(s_devices));

    // Brute-force scan of bus 0 only.
    //
    // A full scan of 256 buses costs 256 x 32 x 8 configuration reads, which is
    // slow, and on QEMU everything we need is on bus 0. Additional buses are
    // reached through PCI-to-PCI bridges, which is where the scan properly
    // belongs once there is a driver model to hang it on — see the note in
    // docs/AfriyieOS-Blueprint.md about the v0.5 PCI service.
    for (af_u8 slot = 0; slot < 32; slot++) {
        probe_slot(0, slot);
    }

    af_info("pci", "found %u device(s) on bus 0", s_device_count);

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Lookups
// -----------------------------------------------------------------------------
const af_pci_device_t *pci_find_class(af_u8 class_code, af_u8 subclass)
{
    for (af_u32 i = 0; i < s_device_count; i++) {
        if (s_devices[i].class_code != class_code) {
            continue;
        }
        if (subclass != 0xFF && s_devices[i].subclass != subclass) {
            continue;
        }
        return &s_devices[i];
    }
    return NULL;
}

const af_pci_device_t *pci_find_device(af_u16 vendor_id, af_u16 device_id)
{
    for (af_u32 i = 0; i < s_device_count; i++) {
        if (s_devices[i].vendor_id == vendor_id &&
            s_devices[i].device_id == device_id) {
            return &s_devices[i];
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// BAR decoding
// -----------------------------------------------------------------------------
af_status_t pci_read_bar(const af_pci_device_t *dev, af_u32 bar_index,
                         af_pci_bar_t *out)
{
    if (dev == NULL || out == NULL || bar_index > 5) {
        return AF_ERR_INVAL;
    }

    af_memset(out, 0, sizeof(*out));

    af_u8 offset = (af_u8)(0x10 + bar_index * 4);
    af_u32 original = af_pci_read32(dev->address, offset);

    // An all-zero BAR is unimplemented. Note that 0 is also a legal I/O base,
    // but no real device uses it, and treating it as absent is what every
    // implementation does.
    if (original == 0) {
        return AF_ERR_NOENT;
    }

    out->is_io = (original & 0x1) != 0;

    // Probe the size: write all ones, read back the bits the device leaves as
    // zero. Those zero bits are the address bits it cannot decode, which gives
    // the size directly. This is the only portable way to ask a device how big
    // its BAR is; the alternative is a hardcoded table per device.
    af_pci_write32(dev->address, offset, 0xFFFFFFFF);
    af_u32 probed = af_pci_read32(dev->address, offset);

    // Restore the original value, whatever we learned.
    af_pci_write32(dev->address, offset, original);

    if (probed == 0 || probed == 0xFFFFFFFF) {
        return AF_ERR_NOTSUP;
    }

    if (!out->is_io) {
        // Memory BARs use bits 3:1 as type flags (64-bit, prefetchable) and
        // bits 15:4 are reserved, so the address mask ignores them.
        out->is_64bit = ((original & 0x6) == 0x4);

        af_u32 mask = probed & 0xFFFFFFF0u;
        out->size    = (af_u64)(~mask) + 1;
        out->address = (af_u64)(original & 0xFFFFFFF0u);

        if (out->is_64bit && bar_index < 5) {
            af_u32 upper = af_pci_read32(dev->address, (af_u8)(offset + 4));
            out->address |= ((af_u64)upper << 32);
        }
    } else {
        af_u32 mask = probed & 0xFFFFFFFCu;
        out->size    = (af_u64)(~mask) + 1;
        out->address = (af_u64)(original & 0xFFFFFFFCu);
    }

    out->valid = (out->size != 0);
    return out->valid ? AF_OK : AF_ERR_NOTSUP;
}

// -----------------------------------------------------------------------------
// Command register control
// -----------------------------------------------------------------------------
static void command_set_bits(const af_pci_device_t *dev, af_u16 bits)
{
    af_u16 command = af_pci_read16(dev->address, 0x04);
    af_pci_write16(dev->address, 0x04, (af_u16)(command | bits));
}

void pci_enable_bus_master(const af_pci_device_t *dev)
{
    if (dev == NULL) {
        return;
    }
    // Bit 2. Without it the device cannot initiate DMA, and a virtqueue
    // submission is accepted and then never completes — which presents as a
    // driver that hangs on its first request.
    command_set_bits(dev, 1u << 2);
}

void pci_enable_memory_space(const af_pci_device_t *dev)
{
    if (dev == NULL) {
        return;
    }
    // Bit 1. Without it, MMIO reads return 0xFFFFFFFF and writes go nowhere.
    command_set_bits(dev, 1u << 1);
}

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------
const char *pci_class_name(af_u8 class_code, af_u8 subclass)
{
    switch (class_code) {
    case AF_PCI_CLASS_STORAGE:
        switch (subclass) {
        case AF_PCI_SUBCLASS_IDE:   return "storage/IDE";
        case AF_PCI_SUBCLASS_SATA:  return "storage/SATA";
        case AF_PCI_SUBCLASS_NVME:  return "storage/NVMe";
        case AF_PCI_SUBCLASS_OTHER: return "storage/other";
        default:                    return "storage";
        }
    case AF_PCI_CLASS_NETWORK:    return "network";
    case AF_PCI_CLASS_DISPLAY:    return "display";
    case AF_PCI_CLASS_BRIDGE:
        return (subclass == AF_PCI_SUBCLASS_PCI2PCI) ? "bridge/PCI-to-PCI"
                                                     : "bridge";
    case AF_PCI_CLASS_SERIAL_BUS: return "serial bus";
    default:                      return "unknown";
    }
}

void pci_dump_devices(void)
{
    if (s_device_count == 0) {
        af_info("pci", "no devices enumerated");
        return;
    }

    af_info("pci", "  bus:slot.func  vendor:device  class             irq");

    for (af_u32 i = 0; i < s_device_count; i++) {
        const af_pci_device_t *d = &s_devices[i];

        // A vendor and device ID pair is conventionally written as
        // 0xVVVV:0xDDDD, which is how every other tool prints it.
        af_info("pci", "  %02u:%02u.%u       %04X:%04X      %-16s  %u",
                d->address.bus, d->address.slot, d->address.func,
                d->vendor_id, d->device_id,
                pci_class_name(d->class_code, d->subclass),
                d->interrupt_line);
    }
}

// SPDX-License-Identifier: MIT
// AfriyieOS — PCI bus enumeration

#ifndef AFRIYIE_PCI_H
#define AFRIYIE_PCI_H

#include "types.h"
#include "status.h"

// -----------------------------------------------------------------------------
// Device address
//
// Bus, device and function identify a device on the bus; the whole of PCI
// configuration space is addressed by that triple plus a register offset.
// -----------------------------------------------------------------------------
typedef struct {
    af_u8  bus;
    af_u8  slot;       // "device" in the specification's wording
    af_u8  func;
    af_u8  _pad;
} af_pci_address_t;

// Class codes we care about. The full list is enormous; these are the ones the
// kernel needs to recognise, and anything else is logged and ignored.
#define AF_PCI_CLASS_STORAGE      0x01
#define AF_PCI_SUBCLASS_IDE       0x01
#define AF_PCI_SUBCLASS_SATA      0x06
#define AF_PCI_SUBCLASS_NVME      0x08
#define AF_PCI_SUBCLASS_OTHER     0x80

#define AF_PCI_CLASS_NETWORK      0x02
#define AF_PCI_CLASS_DISPLAY      0x03
#define AF_PCI_CLASS_BRIDGE       0x06
#define AF_PCI_SUBCLASS_PCI2PCI   0x04
#define AF_PCI_CLASS_SERIAL_BUS   0x0C

typedef struct {
    af_pci_address_t address;
    af_u16 vendor_id;
    af_u16 device_id;
    af_u8  revision;
    af_u8  prog_if;
    af_u8  subclass;
    af_u8  class_code;
    af_u8  header_type;
    af_u8  interrupt_line;
    af_u8  interrupt_pin;
    af_u8  _pad;
    af_u32 bar[6];              // raw BAR values, before size probing
} af_pci_device_t;

// Represents one BAR after its size has been probed.
typedef struct {
    af_u64 address;             // base address (I/O port or MMIO)
    af_u64 size;                // size in bytes
    bool   is_io;               // true = port I/O, false = memory-mapped
    bool   is_64bit;
    bool   valid;
} af_pci_bar_t;

#define AF_PCI_MAX_DEVICES 64

// -----------------------------------------------------------------------------
// Configuration space access (the legacy 0xCF8/0xCFC mechanism)
// -----------------------------------------------------------------------------
af_u32 af_pci_read32(af_pci_address_t addr, af_u8 offset);
af_u16 af_pci_read16(af_pci_address_t addr, af_u8 offset);
af_u8  af_pci_read8(af_pci_address_t addr, af_u8 offset);

void af_pci_write32(af_pci_address_t addr, af_u8 offset, af_u32 value);
void af_pci_write16(af_pci_address_t addr, af_u8 offset, af_u16 value);

// -----------------------------------------------------------------------------
// Enumeration
// -----------------------------------------------------------------------------
af_status_t pci_init(void);

af_u32 pci_device_count(void);
const af_pci_device_t *pci_device(af_u32 index);

// First device matching a class and subclass, or NULL. `subclass` of 0xFF
// matches any subclass.
const af_pci_device_t *pci_find_class(af_u8 class_code, af_u8 subclass);

// First device with an exact vendor and device ID, or NULL. Used for virtio,
// where the device ID encodes which virtio device it is.
const af_pci_device_t *pci_find_device(af_u16 vendor_id, af_u16 device_id);

// Decodes one BAR, probing its size. Sizes are found by the standard trick of
// writing all ones and reading back which bits the device claims are
// unimplemented — the one reliable way to ask a device how large it is.
af_status_t pci_read_bar(const af_pci_device_t *dev, af_u32 bar_index,
                         af_pci_bar_t *out);

// Enables memory space and bus-mastering for a device. Bus mastering must be on
// before the device can perform DMA, and a driver that forgets it sees requests
// that are accepted and then never complete.
void pci_enable_bus_master(const af_pci_device_t *dev);
void pci_enable_memory_space(const af_pci_device_t *dev);

void pci_dump_devices(void);

const char *pci_class_name(af_u8 class_code, af_u8 subclass);

#endif // AFRIYIE_PCI_H

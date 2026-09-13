// SPDX-License-Identifier: MIT
// AfriyieOS — the boot handoff structure
//
// =============================================================================
// THIS IS THE MOST IMPORTANT STRUCTURE IN THE SYSTEM.
// =============================================================================
//
// Whether AfriyieOS was started by UEFI on a PC or by U-Boot on a phone, the
// boot bridge normalises everything the kernel needs into exactly one instance
// of af_boot_info, placed in memory that survives the transition to kernel
// control. The kernel never inspects firmware-specific structures.
//
// Contract:
//   1. The boot bridge must fill every field and set `magic` and `version`.
//   2. The structure is passed to kmain() both in a register AND at a known
//      fixed physical address (AF_BOOT_INFO_BACKUP_ADDR) as a fallback.
//   3. The kernel must treat all pointers as physical addresses until the
//      direct map is installed.

#ifndef AFRIYIE_BOOT_INFO_H
#define AFRIYIE_BOOT_INFO_H

#include "types.h"
#include "config.h"
#include "status.h"   // af_status_t, used by the validation entry points below

// -----------------------------------------------------------------------------
// Identification
// -----------------------------------------------------------------------------
#define AF_BOOT_INFO_MAGIC      0x4146524F20212121ULL  // "AFRO!!!" — AfriyieOS
#define AF_BOOT_INFO_VERSION    1

// Where the UEFI bridge leaves a copy, in case the register path is lost.
// Chosen from the conventional low-memory scratch area, well clear of the
// EBDA (0x9FC00) and the VGA frame buffer (0xA0000).
#define AF_BOOT_INFO_BACKUP_ADDR  0x0000000000007000ULL

// -----------------------------------------------------------------------------
// Firmware origin
// -----------------------------------------------------------------------------
typedef enum {
    AF_FIRMWARE_UNKNOWN = 0,
    AF_FIRMWARE_UEFI    = 1,   // PC
    AF_FIRMWARE_UBOOT   = 2,   // phone / ARM64 board
    AF_FIRMWARE_LK       = 3,  // Little Kernel (alternative phone loader)
    AF_FIRMWARE_MULTIBOOT = 4, // possible future BIOS/GRUB path
} af_firmware_t;

// -----------------------------------------------------------------------------
// Memory map
//
// Normalised across UEFI's EFI_MEMORY_DESCRIPTOR and the device tree's
// /memory + /reserved-memory nodes. The kernel's PMM consumes only this.
// -----------------------------------------------------------------------------
typedef enum {
    AF_MEM_USABLE       = 0,   // free RAM: the PMM may allocate from this
    AF_MEM_RESERVED     = 1,   // firmware-reserved: never touch
    AF_MEM_ACPI_RECLAIM = 2,   // usable after ACPI tables are parsed
    AF_MEM_ACPI_NVS     = 3,   // must be preserved
    AF_MEM_BAD          = 4,   // known-bad RAM
    AF_MEM_BOOTLOADER   = 5,   // loader code/data: reclaimable after handoff
    AF_MEM_KERNEL       = 6,   // our own image, stacks and boot_info
    AF_MEM_FRAMEBUFFER  = 7,   // display memory: never allocate from this
    AF_MEM_RUNTIME      = 8,   // runtime services code/data
    AF_MEM_TABLE        = 9,   // ACPI/firmware tables
} af_memory_type_t;

typedef struct {
    af_paddr           base;      // physical start address, page aligned
    af_u64             length;    // length in bytes, page aligned
    af_memory_type_t   type;      // from the enum above
    af_u32             _reserved; // pad to 24 bytes for the whole struct
} AF_PACKED af_memory_region_t;

AF_STATIC_ASSERT_SIZE(af_memory_region_t, 24);

#define AF_MAX_MEMORY_REGIONS 128

// -----------------------------------------------------------------------------
// Framebuffer (from UEFI GOP or the device tree's simple-framebuffer node)
// -----------------------------------------------------------------------------
// Pixel formats.
//
// The names are deliberately unambiguous about the INTEGER layout of a 32-bit
// pixel as stored little-endian, because UEFI's own names describe the byte
// order in memory and are a well-known source of red/blue swaps:
//
//   AF_PIXEL_RGBX8888  value 0x00RRGGBB, memory bytes B,G,R,X
//                      == UEFI PixelBlueGreenRedReserved8BitPerColor
//   AF_PIXEL_BGRX8888  value 0x00BBGGRR, memory bytes R,G,B,X
//                      == UEFI PixelRedGreenBlueReserved8BitPerColor
typedef enum {
    AF_PIXEL_BGRX8888 = 0,  // integer 0x00BBGGRR (UEFI PixelRedGreenBlue...)
    AF_PIXEL_RGBX8888 = 1,  // integer 0x00RRGGBB (UEFI PixelBlueGreenRed...)
    AF_PIXEL_RGB565   = 2,  // phone panels / simplefb with 16bpp
    AF_PIXEL_INDEXED  = 3,  // palette modes — not supported, reported for diagnosis
} af_pixel_format_t;

typedef struct {
    af_paddr            address;    // physical base of the linear framebuffer
    af_u64              size;       // total mapped size in bytes
    af_u32              width;      // visible width in pixels
    af_u32              height;     // visible height in pixels
    af_u32              pitch;      // bytes per scan line (NOT width * bpp/8)
    af_u16              bpp;        // bits per pixel
    af_u16              red_mask_size,   red_mask_shift;
    af_u16              green_mask_size, green_mask_shift;
    af_u16              blue_mask_size,  blue_mask_shift;
    af_u16              _pad;
    af_pixel_format_t   format;
    af_u32              _reserved;
} AF_PACKED af_boot_framebuffer_t;

// 8 (address) + 8 (size) + 3x4 (geometry) + 7x2 (bpp, masks, pad) + 4 (format)
// + 4 (_reserved) = 52 bytes, with no padding because the struct is packed.
AF_STATIC_ASSERT_SIZE(af_boot_framebuffer_t, 52);

// -----------------------------------------------------------------------------
// Console / debug output
// -----------------------------------------------------------------------------
#define AF_BOOT_CMDLINE_MAX 256

typedef struct {
    af_u64  uart_base;      // physical address of the debug UART (0 = none)
    af_u32  uart_baud;      // requested baud rate
    af_u32  uart_clock_hz;  // input clock, needed for the ARM64 PL011 divisor
    af_u32  uart_reg_shift; // DT reg-shift (0 for most SoCs, 2 for some)
    af_u32  _reserved;
} AF_PACKED af_boot_console_t;

AF_STATIC_ASSERT_SIZE(af_boot_console_t, 24);

// -----------------------------------------------------------------------------
// Address of firmware metadata the kernel may parse later (ACPI RSDP on PC,
// device tree blob on phones). Zero means "not provided".
// -----------------------------------------------------------------------------
typedef struct {
    af_paddr rsdp;          // ACPI RSDP physical address (x86_64)
    af_paddr dtb;           // device tree blob physical address (aarch64)
    af_u64   dtb_size;      // size of the DTB, 0 if unknown
} AF_PACKED af_boot_firmware_t;

AF_STATIC_ASSERT_SIZE(af_boot_firmware_t, 24);

// -----------------------------------------------------------------------------
// The one structure
// -----------------------------------------------------------------------------
typedef struct {
    // --- header: validate this first ---
    af_u64              magic;              // AF_BOOT_INFO_MAGIC
    af_u32              version;            // AF_BOOT_INFO_VERSION
    af_u32              size;               // sizeof(af_boot_info_t)

    // --- origin ---
    af_firmware_t       firmware;           // UEFI / U-Boot / LK / unknown
    af_u32              boot_flags;         // AF_BOOT_FLAG_* bitfield

    // --- platform identity ---
    af_u32              cpu_count;          // logical CPUs the loader saw
    af_u32              page_size;          // firmware page size (informational)
    af_u64              cpu_features;       // arch-specific feature bits

    // --- kernel image placement ---
    af_paddr            kernel_phys_base;   // where the loader put our image
    af_u64              kernel_phys_size;   // size of the loaded image
    af_paddr            ramdisk_phys_base;  // initramfs, 0 if none
    af_u64              ramdisk_size;

    // --- subsystems ---
    af_boot_framebuffer_t framebuffer;
    af_boot_console_t     console;
    af_boot_firmware_t    firmware_info;

    // --- memory map ---
    af_u32              memory_region_count;
    af_u32              _mem_pad;
    af_memory_region_t  memory_regions[AF_MAX_MEMORY_REGIONS];

    // --- performance ---
    af_u64              tsc_per_second;     // x86_64 TSC frequency, 0 if unknown
    af_u64              load_tsc;           // TSC/cycle count at handoff

    // --- free-form ---
    char                cmdline[AF_BOOT_CMDLINE_MAX];
} AF_PACKED af_boot_info_t;

// --- boot_flags bits ---
#define AF_BOOT_FLAG_FRAMEBUFFER   (1u << 0)
#define AF_BOOT_FLAG_UART          (1u << 1)
#define AF_BOOT_FLAG_ACPI          (1u << 2)
#define AF_BOOT_FLAG_DTB           (1u << 3)
#define AF_BOOT_FLAG_RAMDISK       (1u << 4)
#define AF_BOOT_FLAG_SECURE_BOOT   (1u << 5)

// --- firmware-reported machine identity, informational only ---
typedef struct {
    char  vendor[64];
    char  product[64];
    char  version[32];
    char  bootloader[32];
} af_machine_id_t;

// -----------------------------------------------------------------------------
// Validation and reporting
// -----------------------------------------------------------------------------

// Validates magic, version and structure size. Returns AF_OK or one of
// AF_ERR_BOOT_*. Never dereferences past `size` bytes.
af_status_t af_boot_info_validate(const af_boot_info_t *bi);

// Prints a human-readable summary of the boot handoff over the serial console.
// Called by kmain() immediately after validation succeeds.
void af_boot_info_dump(const af_boot_info_t *bi);

// Total usable RAM in bytes, summed over AF_MEM_USABLE regions.
af_u64 af_boot_info_usable_bytes(const af_boot_info_t *bi);

// Highest physical address present in the map (used to size the PMM bitmap).
af_paddr af_boot_info_max_address(const af_boot_info_t *bi);

// Highest address the PMM could ever hand out: the end of the highest region
// that is USABLE or BOOTLOADER-reclaimable.
//
// This is the right figure for sizing the frame bitmap, and it is very different
// from af_boot_info_max_address(). Firmware memory maps routinely describe
// device and reserved windows far above RAM — QEMU's map tops out at 1 TiB on a
// 2 GiB machine. Sizing from the overall maximum produced a 32 MiB bitmap and a
// 256 MiB refcount array for 2 GiB of RAM: 288 MiB of metadata, and initialisation
// loops that walked 268 million frames that can never be allocated.
af_paddr af_boot_info_max_usable_address(const af_boot_info_t *bi);

// Finds the memory region containing `addr`, or NULL.
const af_memory_region_t *af_boot_info_find_region(const af_boot_info_t *bi,
                                                   af_paddr addr);

// Human-readable name for a memory type.
const char *af_memory_type_name(af_memory_type_t type);
const char *af_firmware_name(af_firmware_t firmware);
const char *af_pixel_format_name(af_pixel_format_t format);

#endif // AFRIYIE_BOOT_INFO_H

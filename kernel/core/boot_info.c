// SPDX-License-Identifier: MIT
// AfriyieOS — boot handoff validation and reporting

#include "afriyie/boot_info.h"
#include "afriyie/log.h"
#include "afriyie/kstring.h"

// -----------------------------------------------------------------------------
// Lookup tables
// -----------------------------------------------------------------------------

static const char *const k_memory_type_names[] = {
    [AF_MEM_USABLE]       = "usable",
    [AF_MEM_RESERVED]     = "reserved",
    [AF_MEM_ACPI_RECLAIM] = "acpi-reclaim",
    [AF_MEM_ACPI_NVS]     = "acpi-nvs",
    [AF_MEM_BAD]          = "bad",
    [AF_MEM_BOOTLOADER]   = "bootloader",
    [AF_MEM_KERNEL]       = "kernel",
    [AF_MEM_FRAMEBUFFER]  = "framebuffer",
    [AF_MEM_RUNTIME]      = "runtime",
    [AF_MEM_TABLE]        = "table",
};

const char *af_memory_type_name(af_memory_type_t type)
{
    if ((af_u32)type < AF_ARRAY_LEN(k_memory_type_names)) {
        const char *name = k_memory_type_names[type];
        if (name != NULL) {
            return name;
        }
    }
    return "unknown";
}

const char *af_firmware_name(af_firmware_t firmware)
{
    switch (firmware) {
    case AF_FIRMWARE_UEFI:      return "UEFI";
    case AF_FIRMWARE_UBOOT:     return "U-Boot";
    case AF_FIRMWARE_LK:        return "Little Kernel";
    case AF_FIRMWARE_MULTIBOOT: return "Multiboot";
    case AF_FIRMWARE_UNKNOWN:
    default:                    return "unknown";
    }
}

const char *af_pixel_format_name(af_pixel_format_t format)
{
    switch (format) {
    case AF_PIXEL_BGRX8888: return "BGRX8888";
    case AF_PIXEL_RGBX8888: return "RGBX8888";
    case AF_PIXEL_RGB565:   return "RGB565";
    case AF_PIXEL_INDEXED:  return "indexed";
    default:                return "invalid";
    }
}

// -----------------------------------------------------------------------------
// Validation
//
// This runs before anything else in kmain(). A malformed boot_info must produce
// a clear diagnostic, never a page fault, so every access is bounded by the
// size the loader claimed.
// -----------------------------------------------------------------------------
af_status_t af_boot_info_validate(const af_boot_info_t *bi)
{
    if (bi == NULL) {
        return AF_ERR_INVAL;
    }

    if (bi->magic != AF_BOOT_INFO_MAGIC) {
        return AF_ERR_BOOT_MAGIC;
    }

    if (bi->version != AF_BOOT_INFO_VERSION) {
        return AF_ERR_BOOT_VERSION;
    }

    // The loader must have told us how big its structure is, and it must be at
    // least as large as everything we are about to read.
    if (bi->size < sizeof(af_boot_info_t)) {
        return AF_ERR_BOOT_VERSION;
    }

    if (bi->memory_region_count == 0) {
        return AF_ERR_BOOT_MEMMAP;
    }

    // More regions than we can hold means the loader violated the contract.
    // Truncating silently would hide a firmware bug, so this is an error.
    if (bi->memory_region_count > AF_MAX_MEMORY_REGIONS) {
        return AF_ERR_BOOT_MEMMAP;
    }

    // The memory map must describe at least one usable, page-aligned region
    // that is not zero-length, otherwise there is nowhere to run.
    bool has_usable = false;
    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        const af_memory_region_t *r = &bi->memory_regions[i];

        if (r->length == 0) {
            return AF_ERR_BOOT_MEMMAP;
        }
        if (!AF_IS_ALIGNED(r->base, AF_PAGE_SIZE) ||
            !AF_IS_ALIGNED(r->length, AF_PAGE_SIZE)) {
            return AF_ERR_BOOT_MEMMAP;
        }
        if (r->type == AF_MEM_USABLE) {
            has_usable = true;
        }
    }

    if (!has_usable) {
        return AF_ERR_BOOT_MEMMAP;
    }

    if ((bi->boot_flags & AF_BOOT_FLAG_FRAMEBUFFER) != 0) {
        const af_boot_framebuffer_t *fb = &bi->framebuffer;

        if (fb->address == 0 || fb->width == 0 || fb->height == 0) {
            return AF_ERR_BOOT_NOFB;
        }
        // Pitch is the single most common source of skewed output; validate it
        // rather than trusting it.
        if (fb->bpp == 0) {
            return AF_ERR_BOOT_NOFB;
        }
        af_u32 min_pitch = (fb->width * fb->bpp) / 8u;
        if (fb->pitch < min_pitch) {
            return AF_ERR_BOOT_NOFB;
        }
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------
af_u64 af_boot_info_usable_bytes(const af_boot_info_t *bi)
{
    if (bi == NULL) {
        return 0;
    }

    af_u64 total = 0;
    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        if (bi->memory_regions[i].type == AF_MEM_USABLE ||
            bi->memory_regions[i].type == AF_MEM_BOOTLOADER) {
            total += bi->memory_regions[i].length;
        }
    }
    return total;
}

af_paddr af_boot_info_max_address(const af_boot_info_t *bi)
{
    af_paddr max = 0;

    if (bi == NULL) {
        return 0;
    }

    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        af_paddr end = bi->memory_regions[i].base + bi->memory_regions[i].length;
        if (end > max) {
            max = end;
        }
    }
    return max;
}

const af_memory_region_t *af_boot_info_find_region(const af_boot_info_t *bi,
                                                   af_paddr addr)
{
    if (bi == NULL) {
        return NULL;
    }

    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        const af_memory_region_t *r = &bi->memory_regions[i];
        if (addr >= r->base && addr < r->base + r->length) {
            return r;
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Reporting — the first thing a boot log should contain
// -----------------------------------------------------------------------------
void af_boot_info_dump(const af_boot_info_t *bi)
{
    if (bi == NULL) {
        af_error("boot", "boot_info is NULL");
        return;
    }

    af_info("boot", "==============================================================");
    af_info("boot", " %s %s (%s)", AF_NAME, AF_VERSION_STRING, AF_CODENAME);
    af_info("boot", "==============================================================");
    af_info("boot", "firmware        : %s", af_firmware_name(bi->firmware));
    af_info("boot", "boot flags      : 0x%X", bi->boot_flags);
    af_info("boot", "cpus            : %u", bi->cpu_count);
    af_info("boot", "firmware page   : %u bytes", bi->page_size);
    af_info("boot", "kernel phys     : 0x%X (%u KiB)",
            bi->kernel_phys_base, bi->kernel_phys_size / AF_KIB);

    if ((bi->boot_flags & AF_BOOT_FLAG_RAMDISK) != 0) {
        af_info("boot", "ramdisk         : 0x%X (%u KiB)",
                bi->ramdisk_phys_base, bi->ramdisk_size / AF_KIB);
    } else {
        af_info("boot", "ramdisk         : none");
    }

    // --- console -------------------------------------------------------------
    if ((bi->boot_flags & AF_BOOT_FLAG_UART) != 0) {
        af_info("boot", "debug uart      : 0x%X @ %u baud (clk %u Hz, shift %u)",
                bi->console.uart_base, bi->console.uart_baud,
                bi->console.uart_clock_hz, bi->console.uart_reg_shift);
    } else {
        af_info("boot", "debug uart      : none (screen only)");
    }

    // --- framebuffer ---------------------------------------------------------
    if ((bi->boot_flags & AF_BOOT_FLAG_FRAMEBUFFER) != 0) {
        const af_boot_framebuffer_t *fb = &bi->framebuffer;
        af_info("boot", "framebuffer     : 0x%X  %ux%u  %u bpp  pitch %u  %s",
                fb->address, fb->width, fb->height, fb->bpp, fb->pitch,
                af_pixel_format_name(fb->format));

        // A pitch wider than the visible width is legal and common (alignment
        // padding). Saying so explicitly saves hours of confusion later.
        af_u32 expected = (fb->width * fb->bpp) / 8u;
        if (fb->pitch != expected) {
            af_warn("boot", "framebuffer pitch has %u bytes of padding per line "
                            "(stride, not width, must be used)",
                    fb->pitch - expected);
        }
    } else {
        af_warn("boot", "framebuffer     : none — serial console only");
    }

    // --- firmware metadata ---------------------------------------------------
    if ((bi->boot_flags & AF_BOOT_FLAG_ACPI) != 0) {
        af_info("boot", "acpi rsdp       : 0x%X", bi->firmware_info.rsdp);
    }
    if ((bi->boot_flags & AF_BOOT_FLAG_DTB) != 0) {
        af_info("boot", "device tree     : 0x%X (%u bytes)",
                bi->firmware_info.dtb, bi->firmware_info.dtb_size);
    }

    // --- memory map ----------------------------------------------------------
    af_u64 usable   = af_boot_info_usable_bytes(bi);
    af_paddr top    = af_boot_info_max_address(bi);

    af_info("boot", "memory regions  : %u", bi->memory_region_count);
    af_info("boot", "usable ram      : %u MiB", usable / AF_MIB);
    af_info("boot", "highest address : 0x%X", top);

    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        const af_memory_region_t *r = &bi->memory_regions[i];
        af_info("boot", "  [%2u] 0x%011X - 0x%011X  %8u KiB  %s",
                i, r->base, r->base + r->length - 1,
                (af_u32)(r->length / AF_KIB), af_memory_type_name(r->type));
    }

    if (bi->cmdline[0] != '\0') {
        af_info("boot", "cmdline         : %s", bi->cmdline);
    }
    af_info("boot", "==============================================================");
}

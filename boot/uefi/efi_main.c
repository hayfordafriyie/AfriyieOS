// SPDX-License-Identifier: MIT
// AfriyieOS — UEFI boot bridge
//
// =============================================================================
// WHAT THIS PROGRAM DOES
// =============================================================================
//   1.  Say hello on the firmware text console
//   2.  Disable the firmware watchdog (a boot that takes a moment must not be
//       rebooted underneath itself)
//   3.  Locate the Graphics Output Protocol and record the linear framebuffer
//   4.  Reserve physical memory at KERNEL_PHYS_BASE and copy the kernel image
//       there — the image is embedded in this application's .rodata by
//       embed.asm, so there is no file to open and nothing to go wrong
//   5.  Read the UEFI memory map and normalise it into af_memory_region_t
//   6.  Build af_boot_info in a static buffer, and mirror it to the fixed
//       fallback address the kernel knows about
//   7.  ExitBootServices — retrying with a fresh MapKey if the firmware moved
//       the map underneath us, which it is entitled to do
//   8.  Jump, with paging off and interrupts disabled, to the kernel entry
//
// Everything below step 7 runs with no firmware services whatsoever. Every
// value the kernel needs must therefore already be in af_boot_info.
// =============================================================================

#include "efi.h"
#include "afriyie/boot_info.h"
#include "afriyie/config.h"

// -----------------------------------------------------------------------------
// Linker-provided: where the embedded kernel image starts and ends.
// Defined in embed.asm.
// -----------------------------------------------------------------------------
extern const af_u8 af_kernel_image_start[];
extern const af_u8 af_kernel_image_end[];

// The kernel's entry point, linked at KERNEL_PHYS_BASE.
//
// =============================================================================
// sysv_abi IS NOT OPTIONAL HERE
// =============================================================================
// This source is compiled with the Microsoft x64 ABI, because that is what UEFI
// uses for efi_main. Under that ABI the first integer argument goes in rcx.
//
// The kernel entry stub is compiled for the SysV ABI, where the first argument
// goes in rdi. A plain call therefore puts boot_info in rcx, the stub reads a
// stale rdi, and the kernel reports "boot_info invalid at the supplied pointer".
// It then falls back to the fixed 0x7000 copy — which is why the very first
// successful boot worked at all, and why that fallback path exists.
//
// Declaring the function pointer sysv_abi makes the compiler place the argument
// where the kernel actually looks for it, and align the stack the way SysV
// requires. The explicit fallback stays in kmain regardless: it costs nothing
// and it turned a would-be silent hang into a recoverable boot.
// =============================================================================
typedef void (__attribute__((sysv_abi)) *af_kernel_entry_fn)(af_boot_info_t *boot_info);

// Where the kernel is linked (must match kernel/linker/x86_64.lds).
#define KERNEL_PHYS_BASE  0x100000ULL

// Memory the boot bridge needs to keep alive for the kernel.
// 2 MiB is generous: it covers af_boot_info (about 3.5 KiB), the normalised
// memory map and slack for the firmware's own text.
#define BOOT_RESERVE_SIZE (2 * AF_MIB)

// The UEFI page size is always 4 KiB.
#define EFI_PAGE_SIZE 4096ULL

// -----------------------------------------------------------------------------
// Boot bridge state
//
// Static, so that it lives in this PE image's .data section and therefore
// survives ExitBootServices. Firmware memory is not "given back" to us; we
// simply stop calling services, and the pages this image occupies stay valid.
// -----------------------------------------------------------------------------
static af_boot_info_t   s_boot_info;
static EFI_SYSTEM_TABLE *s_st;

// A simple allocator for the UEFI pool, so we never call AllocatePool after
// ExitBootServices. One buffer, used for the memory map.
static af_u8 s_memmap_storage[64 * 1024];

// -----------------------------------------------------------------------------
// Console output
// -----------------------------------------------------------------------------
static void efi_console_puts(const CHAR16 *text)
{
    if (s_st != NULL && s_st->ConOut != NULL && s_st->ConOut->OutputString != NULL) {
        s_st->ConOut->OutputString(s_st->ConOut, (CHAR16 *)text);
    }
}

// Minimal ASCII to UTF-16 so we can print plain C strings.
//
// Note the two separate indices: `src` walks the ASCII input and `i` walks the
// UTF-16 output. Using one index for both is a real bug that shipped here first:
// translating "\n" into CR+LF advances the output by two but the input by one,
// the two positions desynchronise, and every line after the first is assembled
// from the wrong characters. The symptom is garbled but recognisable text —
// "AryeS010(ed EIbo rde" instead of "AfriyieOS 0.1.0 (Seed)".
static void efi_console_print(const char *ascii)
{
    CHAR16       buffer[256];
    const af_size capacity = (sizeof(buffer) / sizeof(buffer[0])) - 1;
    af_size      i   = 0;
    af_size      src = 0;

    while (ascii[src] != '\0' && i < capacity) {
        char c = ascii[src];

        // The UEFI console needs CRLF. A bare LF stair-steps down the screen.
        if (c == '\n') {
            buffer[i++] = '\r';
            if (i < capacity) {
                buffer[i++] = '\n';
            }
            src++;
            continue;
        }

        buffer[i++] = (CHAR16)(af_u8)c;
        src++;
    }
    buffer[i] = 0;
    efi_console_puts(buffer);
}

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------
static void boot_memset(void *dst, int value, af_size n)
{
    af_u8 *d = (af_u8 *)dst;
    while (n-- > 0) {
        *d++ = (af_u8)value;
    }
}

static void boot_memcpy(void *dst, const void *src, af_size n)
{
    af_u8       *d = (af_u8 *)dst;
    const af_u8 *s = (const af_u8 *)src;
    while (n-- > 0) {
        *d++ = *s++;
    }
}

static void boot_strcpy_safe(char *dst, af_size size, const char *src)
{
    af_size i = 0;
    if (size == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void boot_append_dec(char *buf, af_size size, af_u64 value)
{
    char tmp[24];
    af_size n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }

    af_size len = 0;
    while (buf[len] != '\0' && len + 1 < size) {
        len++;
    }
    while (n > 0 && len + 1 < size) {
        buf[len++] = tmp[--n];
    }
    buf[len] = '\0';
}

// -----------------------------------------------------------------------------
// Step 3: framebuffer via the Graphics Output Protocol
// -----------------------------------------------------------------------------
static EFI_STATUS gop_init(af_boot_info_t *bi)
{
    EFI_GUID gop_guid = AF_EFI_GUID_GRAPHICS_OUTPUT;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

    EFI_STATUS status = s_st->BootServices->LocateProtocol(&gop_guid, NULL,
                                                           (void **)&gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL) {
        efi_console_print("  GOP: not available\n");
        return EFI_NOT_FOUND;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = mode->Info;

    if (info == NULL || mode->FrameBufferBase == 0) {
        efi_console_print("  GOP: no active mode\n");
        return EFI_NOT_FOUND;
    }

    bi->framebuffer.address = mode->FrameBufferBase;
    bi->framebuffer.size    = mode->FrameBufferSize;
    bi->framebuffer.width   = info->HorizontalResolution;
    bi->framebuffer.height  = info->VerticalResolution;
    bi->framebuffer.pitch   = info->PixelsPerScanLine * 4u;
    bi->framebuffer.bpp     = 32;

    switch (info->PixelFormat) {
    case PixelBlueGreenRedReserved8BitPerColor:
        // UEFI names the BYTE order in memory (B,G,R,X). Read as a little-endian
        // 32-bit integer that is 0x00RRGGBB, which is AfriyieOS's RGBX8888.
        // Mapping this the other way round is the classic red/blue swap.
        bi->framebuffer.format = AF_PIXEL_RGBX8888;
        bi->framebuffer.red_mask_size = 8;   bi->framebuffer.red_mask_shift = 16;
        bi->framebuffer.green_mask_size = 8; bi->framebuffer.green_mask_shift = 8;
        bi->framebuffer.blue_mask_size = 8;  bi->framebuffer.blue_mask_shift = 0;
        break;

    case PixelRedGreenBlueReserved8BitPerColor:
        // Memory bytes R,G,B,X => integer 0x00BBGGRR => AfriyieOS BGRX8888.
        bi->framebuffer.format = AF_PIXEL_BGRX8888;
        bi->framebuffer.red_mask_size = 8;   bi->framebuffer.red_mask_shift = 0;
        bi->framebuffer.green_mask_size = 8; bi->framebuffer.green_mask_shift = 8;
        bi->framebuffer.blue_mask_size = 8;  bi->framebuffer.blue_mask_shift = 16;
        break;

    case PixelBitMask:
        // Trust the firmware's masks rather than assuming an order. The shifts
        // are the position of the lowest set bit of each mask.
        bi->framebuffer.red_mask_size   = 8;
        bi->framebuffer.green_mask_size = 8;
        bi->framebuffer.blue_mask_size  = 8;
        bi->framebuffer.red_mask_shift   = (af_u16)__builtin_ctz(info->PixelInformation.RedMask | 1u);
        bi->framebuffer.green_mask_shift = (af_u16)__builtin_ctz(info->PixelInformation.GreenMask | 1u);
        bi->framebuffer.blue_mask_shift  = (af_u16)__builtin_ctz(info->PixelInformation.BlueMask | 1u);

        // Derive the integer layout from the masks: if red is in the low byte
        // the pixel is BGRX, otherwise RGBX.
        bi->framebuffer.format = (bi->framebuffer.red_mask_shift == 0)
                                     ? AF_PIXEL_BGRX8888
                                     : AF_PIXEL_RGBX8888;
        break;

    default:
        // PixelBltOnly: the framebuffer cannot be written directly. The kernel
        // cannot use it in v0.1, so say so rather than drawing into nothing.
        efi_console_print("  GOP: mode is BLT-only, no linear framebuffer\n");
        return EFI_UNSUPPORTED;
    }

    bi->boot_flags |= AF_BOOT_FLAG_FRAMEBUFFER;

    // Report it, including the stride, which is where first attempts usually go
    // wrong.
    efi_console_print("  GOP: ");
    {
        char line[128];
        line[0] = '\0';
        boot_append_dec(line, sizeof(line), info->HorizontalResolution);
        // manual separator
        af_size len = 0;
        while (line[len] != '\0') { len++; }
        if (len + 1 < sizeof(line)) { line[len++] = 'x'; line[len] = '\0'; }
        boot_append_dec(line, sizeof(line), info->VerticalResolution);

        len = 0;
        while (line[len] != '\0') { len++; }
        const char *suffix = " pitch ";
        for (af_size i = 0; suffix[i] != '\0' && len + 1 < sizeof(line); i++) {
            line[len++] = suffix[i];
        }
        line[len] = '\0';
        boot_append_dec(line, sizeof(line), bi->framebuffer.pitch);

        len = 0;
        while (line[len] != '\0') { len++; }
        if (len + 1 < sizeof(line)) { line[len++] = '\n'; line[len] = '\0'; }

        efi_console_print(line);
    }

    return EFI_SUCCESS;
}

// -----------------------------------------------------------------------------
// Step 4: copy the kernel image to its link address
// -----------------------------------------------------------------------------
static EFI_STATUS load_kernel(af_boot_info_t *bi)
{
    af_u64 image_size = (af_u64)(af_kernel_image_end - af_kernel_image_start);
    af_u64 pages = (image_size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;

    // Reserve exactly the range the kernel is linked at. AllocateAddress makes
    // the firmware fail loudly rather than placing the image somewhere else and
    // letting us jump into the wrong memory.
    EFI_PHYSICAL_ADDRESS target = KERNEL_PHYS_BASE;
    EFI_STATUS status = s_st->BootServices->AllocatePages(AllocateAddress, 2,
                                                          pages, &target);
    if (EFI_ERROR(status)) {
        efi_console_print("  kernel: AllocatePages at 0x100000 FAILED\n");
        return status;
    }

    if (target != KERNEL_PHYS_BASE) {
        // AllocateAddress is not supposed to relocate. If it did, jumping to
        // KERNEL_PHYS_BASE would execute garbage.
        efi_console_print("  kernel: firmware relocated the image — refusing\n");
        return EFI_LOAD_ERROR;
    }

    boot_memcpy((void *)(af_uptr)KERNEL_PHYS_BASE, af_kernel_image_start,
                (af_size)image_size);

    bi->kernel_phys_base = KERNEL_PHYS_BASE;
    bi->kernel_phys_size = image_size;

    efi_console_print("  kernel: ");
    {
        char line[64];
        line[0] = '\0';
        boot_append_dec(line, sizeof(line), image_size / 1024);
        af_size len = 0;
        while (line[len] != '\0') { len++; }
        const char *suffix = " KiB loaded at 0x100000\n";
        for (af_size i = 0; suffix[i] != '\0' && len + 1 < sizeof(line); i++) {
            line[len++] = suffix[i];
        }
        line[len] = '\0';
        efi_console_print(line);
    }

    return EFI_SUCCESS;
}

// -----------------------------------------------------------------------------
// Step 5: read and normalise the memory map
// -----------------------------------------------------------------------------
static EFI_STATUS read_memory_map(af_boot_info_t *bi, EFI_UINTN *out_map_key)
{
    EFI_UINTN map_size = 0;
    EFI_UINTN map_key = 0;
    EFI_UINTN descriptor_size = 0;
    af_u32    descriptor_version = 0;

    // First call: intentionally with a zero-size buffer to learn the real size.
    EFI_STATUS status = s_st->BootServices->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)s_memmap_storage,
        &map_key, &descriptor_size, &descriptor_version);

    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
        efi_console_print("  memmap: probe call failed\n");
        return status;
    }

    // The map can grow between the two calls, so leave slack. Firmware that
    // allocates during GetMemoryMap is a well-known trap.
    map_size += 4096;
    if (map_size > sizeof(s_memmap_storage)) {
        efi_console_print("  memmap: does not fit in the boot buffer\n");
        return EFI_BUFFER_TOO_SMALL;
    }

    status = s_st->BootServices->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)s_memmap_storage,
        &map_key, &descriptor_size, &descriptor_version);

    if (EFI_ERROR(status)) {
        efi_console_print("  memmap: second call failed\n");
        return status;
    }

    if (descriptor_size == 0 || descriptor_size > 128) {
        efi_console_print("  memmap: implausible descriptor size\n");
        return EFI_DEVICE_ERROR;
    }

    af_u32 count = (af_u32)(map_size / descriptor_size);
    af_u32 written = 0;

    for (af_u32 i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(s_memmap_storage + (i * descriptor_size));

        if (written >= AF_MAX_MEMORY_REGIONS) {
            efi_console_print("  memmap: more regions than af_boot_info can hold\n");
            return EFI_BUFFER_TOO_SMALL;
        }

        af_memory_type_t type;
        switch (d->Type) {
        case EfiConventionalMemory:      type = AF_MEM_USABLE;       break;
        case EfiLoaderCode:
        case EfiLoaderData:              type = AF_MEM_BOOTLOADER;   break;
        case EfiBootServicesCode:
        case EfiBootServicesData:        type = AF_MEM_BOOTLOADER;   break;
        case EfiACPIReclaimMemory:       type = AF_MEM_ACPI_RECLAIM; break;
        case EfiACPIMemoryNVS:           type = AF_MEM_ACPI_NVS;     break;
        case EfiUnusableMemory:          type = AF_MEM_BAD;          break;
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:     type = AF_MEM_RUNTIME;      break;
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace: type = AF_MEM_RESERVED;     break;
        case EfiReservedMemoryType:
        default:                         type = AF_MEM_RESERVED;     break;
        }

        // Firmware sometimes reports zero-length or sub-page entries. The
        // kernel rejects those outright, so drop them here rather than making
        // the kernel refuse to boot.
        af_u64 length = d->NumberOfPages * EFI_PAGE_SIZE;
        if (length == 0) {
            continue;
        }

        bi->memory_regions[written].base   = d->PhysicalStart;
        bi->memory_regions[written].length = length & ~(EFI_PAGE_SIZE - 1);
        bi->memory_regions[written].type   = type;
        bi->memory_regions[written]._reserved = 0;
        written++;
    }

    bi->memory_region_count = written;
    *out_map_key = map_key;

    return EFI_SUCCESS;
}

// -----------------------------------------------------------------------------
// Step 8: leave firmware behind and enter the kernel
// -----------------------------------------------------------------------------
static AF_NORETURN void jump_to_kernel(af_boot_info_t *bi)
{
    af_kernel_entry_fn entry = (af_kernel_entry_fn)(af_uptr)KERNEL_PHYS_BASE;

    // Firmware is gone from this point. No BootServices calls, no console, no
    // allocations — the kernel is on its own.
    __asm__ __volatile__("cli");

    entry(bi);

    // The kernel never returns. If it somehow does, stop the CPU rather than
    // executing whatever memory follows.
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------
EFI_STATUS EFI_MS_ABI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    s_st = SystemTable;

    efi_console_print("\n");
    efi_console_print("AfriyieOS " AF_VERSION_STRING " (" AF_CODENAME ") UEFI boot bridge\n");
    efi_console_print("--------------------------------------------------------------\n");

    if (s_st == NULL || s_st->BootServices == NULL || s_st->ConOut == NULL) {
        // Nothing we can do and nowhere to say it.
        return EFI_INVALID_PARAMETER;
    }

    // --- Step 2: stop the watchdog -------------------------------------------
    // Firmware arms a 5-minute watchdog by default. A boot that hangs must not
    // be "rescued" by a reset that destroys the evidence.
    s_st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    efi_console_print("  watchdog disabled\n");

    // --- Build the handoff structure -----------------------------------------
    boot_memset(&s_boot_info, 0, sizeof(s_boot_info));
    s_boot_info.magic         = AF_BOOT_INFO_MAGIC;
    s_boot_info.version       = AF_BOOT_INFO_VERSION;
    s_boot_info.size          = (af_u32)sizeof(af_boot_info_t);
    s_boot_info.firmware      = AF_FIRMWARE_UEFI;
    s_boot_info.page_size     = 4096;
    s_boot_info.cpu_count     = 1;         // SMP bring-up arrives in v1.2
    s_boot_info.console.uart_base  = 0x3F8;
    s_boot_info.console.uart_baud  = 115200;
    // Set the flag as well as the fields. Without it the kernel's boot log
    // reports "debug uart: none (screen only)" and silently falls back to the
    // conventional COM1 — which works on a PC and is wrong on any machine whose
    // debug UART is somewhere else.
    s_boot_info.boot_flags        |= AF_BOOT_FLAG_UART;
    boot_strcpy_safe(s_boot_info.cmdline, sizeof(s_boot_info.cmdline),
                     "afriyie console=serial fb=gop");

    // --- Step 3: framebuffer --------------------------------------------------
    EFI_STATUS status = gop_init(&s_boot_info);
    if (EFI_ERROR(status)) {
        efi_console_print("  framebuffer unavailable; the kernel will use serial only\n");
    }

    // --- Step 4: kernel image -------------------------------------------------
    status = load_kernel(&s_boot_info);
    if (EFI_ERROR(status)) {
        efi_console_print("FATAL: could not place the kernel image. Halting.\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }

    // --- Step 5/7: memory map, then ExitBootServices --------------------------
    //
    // =========================================================================
    // NOTHING MAY RUN BETWEEN GetMemoryMap AND ExitBootServices
    // =========================================================================
    // ExitBootServices takes the MapKey that GetMemoryMap returned, and rejects
    // the call if the map has changed since. It changes whenever anything
    // allocates or frees a page.
    //
    // Printing to the UEFI console allocates: the console driver grows buffers
    // as it renders. So a diagnostic "memmap: N regions" line placed between the
    // two calls invalidates the key *every single time*, ExitBootServices fails
    // with EFI_INVALID_PARAMETER, and the retry loop spins until it gives up.
    //
    // That is exactly what happened on the first real boot. The fix is ordering,
    // not a retry: fetch the map and exit boot services back to back, and only
    // talk to the console on the retry path, where the map will be re-read
    // anyway.
    //
    // The kernel prints the full memory map from af_boot_info_dump() a moment
    // later, so no diagnostic information is lost.
    // =========================================================================
    EFI_UINTN map_key = 0;
    bool exited = false;

    for (af_u32 attempt = 0; attempt < 4; attempt++) {
        // No output between here...
        status = read_memory_map(&s_boot_info, &map_key);
        if (EFI_ERROR(status)) {
            efi_console_print("FATAL: could not read the UEFI memory map. Halting.\n");
            for (;;) {
                __asm__ __volatile__("hlt");
            }
        }

        // Mark the framebuffer region so the kernel's PMM never allocates from
        // it. GOP memory is usually already reserved or MMIO, but some firmware
        // reports it as conventional — and losing video memory to the allocator
        // is a nasty, intermittent bug. This touches only our own copy of the
        // map and allocates nothing.
        if ((s_boot_info.boot_flags & AF_BOOT_FLAG_FRAMEBUFFER) != 0) {
            af_paddr fb_start = s_boot_info.framebuffer.address;
            af_paddr fb_end   = fb_start + s_boot_info.framebuffer.size;

            for (af_u32 i = 0; i < s_boot_info.memory_region_count; i++) {
                af_memory_region_t *r = &s_boot_info.memory_regions[i];

                if (r->type == AF_MEM_USABLE &&
                    fb_start < r->base + r->length && r->base < fb_end) {
                    r->type = AF_MEM_FRAMEBUFFER;
                }
            }
        }

        EFI_STATUS exit_status = s_st->BootServices->ExitBootServices(ImageHandle,
                                                                     map_key);
        // ...and here.
        if (!EFI_ERROR(exit_status)) {
            exited = true;
            break;
        }

        // Safe to speak now: the map will be re-read before the next attempt, so
        // whatever this console write allocates does not matter.
        efi_console_print("  ExitBootServices rejected the map key; refetching\n");
    }

    if (!exited) {
        efi_console_print("FATAL: ExitBootServices failed after 4 attempts. Halting.\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }

    // --- Step 6: publish af_boot_info ----------------------------------------
    //
    // Two copies. The register is the fast, correct path; the fixed address is
    // the recovery path the kernel tries if the register is lost on the jump.
    boot_memcpy((void *)(af_uptr)AF_BOOT_INFO_BACKUP_ADDR,
                &s_boot_info, sizeof(s_boot_info));

    // --- Step 8: go -----------------------------------------------------------
    jump_to_kernel(&s_boot_info);
}

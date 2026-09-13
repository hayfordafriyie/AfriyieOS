/* SPDX-License-Identifier: MIT
 * AfriyieOS — UEFI application linker script
 *
 * The boot application is a PE32+ executable. Firmware loads it at an arbitrary
 * address and applies its own relocations, so unlike the kernel it must be
 * position-independent and must NOT be linked at a fixed address.
 *
 * The subsections below are what makes the generated PE/COFF object acceptable
 * to the UEFI loader:
 *   .reloc     — required, because firmware relocates the image
 *   .drectve   — carries the subsystem and entry-point hints
 *   .pdata     — exception directory, expected by some firmware
 */

OUTPUT_FORMAT(pei-x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = SIZEOF_HEADERS;

    . = ALIGN(4096);

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
        /* The embedded kernel image lands here via embed.asm. */
    }

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(4096);

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(4096);

    /* Relocations: firmware may move the image, so this is mandatory. */
    .reloc : {
        *(.reloc)
    }

    /* Directive section: tells the firmware this is an EFI application. */
    .drectve : {
        *(.drectve)
    }

    /* Exception directory. */
    .pdata : {
        *(.pdata)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.debug_*)
    }
}

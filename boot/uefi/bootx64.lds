/* SPDX-License-Identifier: MIT
 * AfriyieOS — UEFI application linker script
 *
 * The boot application is a PE32+ executable. Firmware loads it at an arbitrary
 * address and applies its own relocations, so unlike the kernel it must be
 * position-independent and must NOT be linked at a fixed address.
 *
 * =============================================================================
 * SECTION ORDER MATTERS, AND .reloc MUST BE LAST
 * =============================================================================
 * The UEFI loader walks the base relocation table while the image is being
 * paged in, so `.reloc` has to be the final section in the image. If any
 * allocated section follows it, the linker warns:
 *
 *     base relocation for section `.rdata' above .reloc section
 *
 * and a strict firmware may relocate against a partially loaded image. Every
 * code and data section is therefore listed before `.reloc` below, including the
 * `.pdata`/`.xdata` exception directory that GCC emits for the Windows ABI.
 *
 * Also note the section patterns: mingw's GCC emits `.rdata` where an ELF
 * compiler would emit `.rodata`, and `.rdata$zzz`-style merged names as well.
 * A script that only lists `.rodata` silently leaves them to the default
 * placement rules.
 */

OUTPUT_FORMAT(pei-x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = SIZEOF_HEADERS;
    . = ALIGN(4096);

    /* --- code ------------------------------------------------------------- */
    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);

    /* --- read-only data ---------------------------------------------------- */
    /* The embedded kernel image lands here via embed.asm. */
    .rodata : {
        *(.rodata .rodata.*)
        *(.rdata .rdata.*)
    }

    . = ALIGN(4096);

    /* --- initialised data -------------------------------------------------- */
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(4096);

    /* --- uninitialised data ------------------------------------------------- */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(4096);

    /* --- exception directory (Windows ABI unwind info) --------------------- */
    .pdata : {
        *(.pdata)
    }

    . = ALIGN(4096);
    .xdata : {
        *(.xdata)
    }

    . = ALIGN(4096);

    /* --- linker directives -------------------------------------------------- */
    /* Tells the firmware this is an EFI application. */
    .drectve : {
        *(.drectve)
    }

    . = ALIGN(4096);

    /* --- BASE RELOCATIONS: LAST SECTION IN THE IMAGE ------------------------ */
    .reloc : {
        *(.reloc)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.debug_*)
    }
}

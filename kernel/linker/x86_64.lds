/* SPDX-License-Identifier: MIT
 * AfriyieOS — x86_64 kernel linker script (v0.1)
 *
 * =============================================================================
 * WHY THIS IS AN IDENTITY MAPPING AT 1 MiB, AND NOT THE HIGHER HALF
 * =============================================================================
 *
 * The blueprint's final address-space layout (section 3.3) puts the kernel at
 * 0xFFFFFFFF80000000, the top 2 GiB. That is the right design — but it requires
 * page tables that map the higher half onto physical memory, and v0.1 has no
 * paging at all (paging is milestone v0.2).
 *
 * Jumping into a higher-half image with paging still off faults immediately:
 * the instruction fetch at 0xFFFFFFFF80000000 goes to an unmapped address.
 *
 * So v0.1 links the kernel as an IDENTITY MAPPING at physical 1 MiB, with the
 * default (small) code model. When v0.2 introduces the PMM and VMM, this script
 * gains the higher-half VMA plus a physical load address, a small assembly
 * bootstrap sets up the initial page tables, and the compiler flag changes to
 * -mcmodel=kernel. Both changes are made together, in that commit — see
 * AF_KERNEL_HIGHER_HALF in cmake/flags.cmake.
 *
 * 1 MiB is chosen because everything below it is either legacy firmware area,
 * the EBDA, the VGA window at 0xA0000, or the real-mode IVT. Starting at
 * 0x100000 is the conventional safe base and it matches KERNEL_LMA in the boot
 * bridge.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

KERNEL_BASE = 0x100000;   /* 1 MiB: identity-mapped VMA == LMA in v0.1 */

SECTIONS
{
    . = KERNEL_BASE;

    /* ---------------------------------------------------------------------
     * Text
     * --------------------------------------------------------------------- */
    .text : {
        /* KEEP: the entry stub must survive --gc-sections, because nothing
         * references it — the boot bridge jumps to it by name. */
        KEEP(*(.text.boot))
        KEEP(*(.text.kernel_entry))

        . = ALIGN(16);
        *(.text .text.*)
    }

    . = ALIGN(4096);

    /* ---------------------------------------------------------------------
     * Read-only data
     * --------------------------------------------------------------------- */
    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r*)
    }

    . = ALIGN(4096);

    /* ---------------------------------------------------------------------
     * Initialised data
     * --------------------------------------------------------------------- */
    .data : {
        *(.data .data.*)
        *(.gnu.linkonce.d*)
    }

    /* ---------------------------------------------------------------------
     * Uninitialised data
     *
     * __bss_start and __bss_end are read by kernel/arch/x86_64/entry.asm to
     * zero this range before C code runs. They must stay 8-byte aligned and
     * must bound the whole section.
     * --------------------------------------------------------------------- */
    . = ALIGN(16);
    __bss_start = .;

    .bss : {
        *(.bss .bss.*)
        *(.gnu.linkonce.b*)
        *(COMMON)
    }

    . = ALIGN(16);
    __bss_end = .;

    /* ---------------------------------------------------------------------
     * Image bounds
     * --------------------------------------------------------------------- */
    . = ALIGN(16);
    __kernel_end  = .;
    __kernel_start = KERNEL_BASE;
    __kernel_size = __kernel_end - __kernel_start;

    /* ---------------------------------------------------------------------
     * Non-allocated sections: must not take up image space.
     * --------------------------------------------------------------------- */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.debug_*)
    }
}

/* SPDX-License-Identifier: MIT
 * AfriyieOS — x86_64 kernel linker script
 *
 * The kernel is linked into the TOP 2 GiB of the address space, at
 * 0xFFFFFFFF80000000. This is what makes `-mcmodel=kernel` necessary: the
 * default small code model cannot address anything above 2 GiB, so without it
 * every absolute reference is silently truncated.
 *
 * The image itself is loaded at physical 0x100000 (1 MiB) by the boot bridge,
 * which is why the load address and the link address differ and the script is
 * careful to keep the two in step.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

/* Higher-half base: the top 2 GiB, page aligned. */
KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x100000;                 /* 1 MiB: safely above the legacy area */

SECTIONS
{
    . = KERNEL_VMA;

    /* ---------------------------------------------------------------------
     * Text
     * --------------------------------------------------------------------- */
    .text : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA)
    {
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
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r*)
    }

    . = ALIGN(4096);

    /* ---------------------------------------------------------------------
     * Initialised data
     * --------------------------------------------------------------------- */
    .data : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA)
    {
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

    .bss : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.bss .bss.*)
        *(.gnu.linkonce.b*)
        *(COMMON)
    }

    . = ALIGN(16);
    __bss_end = .;

    /* ---------------------------------------------------------------------
     * Stack
     *
     * v0.1 uses the single stack defined in entry.asm. These symbols exist so
     * that the debug build and the panic handler can report the stack bounds,
     * and so that v0.2 can place per-thread stacks relative to the image end.
     * --------------------------------------------------------------------- */
    . = ALIGN(16);
    __kernel_end = .;

    /* Kernel size, used by the size budget check in CI (blueprint section 14:
     * the kernel must stay under 64 KiB of .text). */
    __kernel_size = __kernel_end - KERNEL_VMA;

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

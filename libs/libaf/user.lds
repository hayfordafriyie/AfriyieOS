/* SPDX-License-Identifier: MIT
 * AfriyieOS — user program linker script
 *
 * Two things here are not negotiable.
 *
 * THE LOAD ADDRESS IS 512 GiB — the base of the user region, which is PML4[1].
 * The kernel's identity map lives in PML4[0], and a user page anywhere in that
 * entry would force the USER bit onto the top-level entry shared with the
 * kernel. See AF_USER_PML4_INDEX in kernel/include/afriyie/config.h: the slot a
 * program is linked into is part of the address-space contract, not an
 * implementation detail, and changing it here without changing the kernel
 * produces a program that loads and then faults.
 *
 * THE SEGMENTS ARE PAGE-ALIGNED. The kernel maps one page at a time, copying
 * each segment's file bytes into the pages it spans. A segment starting
 * mid-page would share a frame with the previous segment, and the second
 * segment's mapping would overwrite the first one's permissions. Aligning each
 * segment to a page costs a few KiB of image and removes the problem entirely.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x0000008000000000;

    .text : ALIGN(4096) {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4096) {
        *(.data .data.*)
    } :data

    /* .bss is part of the data segment but contributes no bytes to the file:
     * p_memsz reaches past p_filesz, and the kernel zeroes the pages in that
     * gap. The program checks that it did. */
    .bss : ALIGN(4096) {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}

/* SPDX-License-Identifier: MIT
 * AfriyieOS — user program linker script
 *
 * Two things here are not negotiable.
 *
 * THE LOAD ADDRESS IS 4 GiB. The kernel identity-maps the low 3 GiB as
 * kernel-only using 2 MiB huge pages. A program loaded inside that range would
 * need those huge pages split, and would sit in address space the kernel has
 * already described to itself as its own. 4 GiB is above the identity map and
 * comfortably inside the user half, so the program's pages are separate from
 * the kernel's on every level of the page table.
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
    . = 0x0000000100000000;

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

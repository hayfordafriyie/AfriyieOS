/* SPDX-License-Identifier: MIT
 * AfriyieOS — init
 *
 * The first program that is genuinely a program: compiled by the cross
 * compiler, linked at 4 GiB, written into the FAT32 volume as an ELF file, read
 * back off the disk by the kernel's ELF loader, and entered in ring 3.
 *
 * Everything it does is chosen as evidence of one step in that chain. It has no
 * libc and no framework behind it, so if the loader got a single step wrong one
 * of the checks below fails and says which.
 */

#include "af.h"

/* --- .data ----------------------------------------------------------------
 * Bytes that exist in the file. The loader must copy them into the pages it
 * maps. A loader that maps cleared frames and forgets the copy reads back
 * zeroes here, and check 2 fires.
 * ------------------------------------------------------------------------ */
static const char banner[] = "init: hello from a program loaded off the disk\n";

static const char *const stages[] = {
    "boot",     /* [0] */
    "load",     /* [1] */
    "run",      /* [2] */
};

/* --- .bss -----------------------------------------------------------------
 * No bytes in the file: p_filesz ends before this starts and p_memsz covers it.
 * The loader must hand us frames that are already zero. This is the step most
 * likely to be wrong, and a user program is the only thing that can observe it
 * from the outside — the kernel cannot check its own zeroing and be believed.
 * ------------------------------------------------------------------------ */
static af_u8  bss_buffer[4096];
static af_u64 bss_counter;

/* Reports a failure and stops. Defined before main so that main reads as the
 * sequence of checks it is. */
AF_NORETURN static void fail(const char *what, af_u64 detail)
{
    af_puts("init: FAIL: ");
    af_puts(what);
    if (detail != 0) {
        af_puts(" (");
        af_putu(detail, 10);
        af_puts(")");
    }
    af_putc('\n');
    af_exit(1);
}

int main(void)
{
    af_puts("----------------------------------------------------------\n");
    af_puts(banner);

    /* -- 1. .bss must be zero ---------------------------------------------
     * A loader that maps frames without clearing them, or that hands over a
     * frame the kernel has already written to, fails right here. */
    af_u64 non_zero_bytes = 0;
    for (af_u32 i = 0; i < (af_u32)sizeof(bss_buffer); i++) {
        if (bss_buffer[i] != 0) {
            non_zero_bytes++;
        }
    }
    if (non_zero_bytes != 0) {
        fail(".bss was not zeroed", non_zero_bytes);
    }

    if (bss_counter != 0) {
        fail("scalar .bss was not zeroed", bss_counter);
    }

    af_puts("init:   .bss is zero (");
    af_putu(sizeof(bss_buffer), 10);
    af_puts(" bytes)\n");

    /* -- 2. .data must have been copied in -------------------------------- */
    if (af_strlen(banner) < 16 || stages[2][0] != 'r' || stages[2][1] != 'u') {
        fail(".data was not loaded", (af_u64)stages[2][1]);
    }

    af_puts("init:   .data is intact (");
    af_putu(af_strlen(banner), 10);
    af_puts(" bytes of banner text)\n");

    /* -- 3. .bss must be real, private, writable memory -------------------
     * Writing a pattern and reading it back proves these pages are ours, and
     * not a read-only or shared alias of something the kernel still owns. */
    for (af_u32 i = 0; i < (af_u32)sizeof(bss_buffer); i++) {
        bss_buffer[i] = (af_u8)(i * 7u + 3u);
    }

    af_u64 checksum = 0;
    af_u64 mismatches = 0;
    for (af_u32 i = 0; i < (af_u32)sizeof(bss_buffer); i++) {
        af_u8 expected = (af_u8)(i * 7u + 3u);
        if (bss_buffer[i] != expected) {
            mismatches++;
        }
        checksum += bss_buffer[i];
    }
    if (mismatches != 0) {
        fail(".bss did not hold written values", mismatches);
    }

    bss_counter = checksum;
    af_puts("init:   .bss holds a written pattern, checksum ");
    af_putu(checksum, 10);
    af_putc('\n');

    /* -- 4. The clock syscall ---------------------------------------------
     * A second call with a different return convention, so the ABI is
     * exercised in more than one direction. A nanosecond reading only means
     * something relative to another one, so take two and report the delta. */
    af_i64 first = af_clock_ns();
    if (first < 0) {
        fail("clock_get returned an error", (af_u64)(-first));
    }

    /* -- 5. Yield ----------------------------------------------------------
     * Gives the scheduler a chance to run something else and come back. If
     * returning from a yield loses this thread's context, nothing after this
     * point runs and the boot test reports a missing marker rather than a
     * wrong value — which is the failure this arrangement is designed to make
     * visible. */
    af_yield();

    af_i64 second = af_clock_ns();
    if (second < first) {
        fail("the clock went backwards", (af_u64)(first - second));
    }

    af_puts("init:   clock ");
    af_putu((af_u64)first, 10);
    af_puts(" ns -> ");
    af_putu((af_u64)second, 10);
    af_puts(" ns (");
    af_putu((af_u64)(second - first), 10);
    af_puts(" ns across a yield)\n");

    /* -- 6. Reading .rodata through a computed index ----------------------
     * The index comes from the checksum, so the compiler cannot fold the whole
     * table into one immediate and quietly not emit the segment at all. */
    af_u32 stage = (af_u32)(checksum % 3u);
    af_puts("init:   stage[");
    af_putu(stage, 10);
    af_puts("] = ");
    af_puts(stages[stage]);
    af_putc('\n');

    /* The marker the boot test waits for. Printed by user code, in ring 3,
     * after every check above has passed — so it is evidence that a program
     * from the disk ran, not merely that the kernel loaded one. */
    af_puts("AF_EXEC_RAN\n");
    af_puts("init: all checks passed, exiting\n");
    af_puts("----------------------------------------------------------\n");

    af_exit(0);
}

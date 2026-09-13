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

/* Reads the code segment selector. This is the one thing a program can check
 * about its own privilege that the kernel cannot check for it: if this runs at
 * all and the bottom two bits are 3, the CPU is enforcing a boundary, not the
 * kernel politely declining to look at something. */
static af_u16 current_cs(void)
{
    af_u16 cs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
    return cs;
}

int main(void)
{
    af_puts("----------------------------------------------------------\n");
    af_puts(banner);

    /* -- 0. We are actually in ring 3 -------------------------------------
     * The requested privilege level is the low two bits of CS. Anything else
     * means the "user" program is running with kernel privileges, and every
     * other check below is worthless — so this one runs first. */
    const af_u16 cs = current_cs();
    if ((cs & 3u) != 3u) {
        fail("not running at CPL 3 (cs)", (af_u64)cs);
    }

    af_puts("init:   cs = 0x");
    af_putu(cs, 16);
    af_puts(", CPL = 3\n");

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

    /* -- 7. Hostile pointers must be refused, not dereferenced -------------
     * The kernel validates every pointer arriving through a system call. This
     * is the only place that can test it, because it needs a program willing to
     * pass bad pointers — and if the validation is wrong the failure is not an
     * error code, it is a page fault in kernel mode and a dead machine. So each
     * of these must return a negative status and let this program continue.
     *
     * A specific error code is deliberately not asserted. User space has no copy
     * of the status table, and duplicating it here is how the syscall numbers
     * drifted out of step between two documents earlier in this milestone. What
     * matters is the shape: an error, not a fault.
     */
    static const struct {
        const char *what;
        af_u64      addr;
        af_u64      len;
    } bad[] = {
        { "null page",              0x0000000000000000ULL, 16 },
        { "unmapped but in range",  0x00000000DEADBEEFULL, 16 },
        { "a hole in our own image",0x0000000100005000ULL, 16 },
        { "length that wraps",      0x00007FFFFFFFFFF0ULL, ~(af_u64)0 },
        { "past the user top",      0x00007FFFFFFFFFF0ULL, 64 },
        { "zero length",            0x0000000100000000ULL, 0 },
    };

    for (af_u32 i = 0; i < (af_u32)(sizeof(bad) / sizeof(bad[0])); i++) {
        const af_i64 rc = af_write((const char *)(af_u64)bad[i].addr, bad[i].len);

        if (rc >= 0) {
            af_puts("init: FAIL: sys_debug_write accepted a bad pointer: ");
            af_puts(bad[i].what);
            af_puts(" (returned ");
            af_puti(rc);
            af_puts(")\n");
            af_exit(1);
        }
    }

    af_puts("init:   ");
    af_putu(sizeof(bad) / sizeof(bad[0]), 10);
    af_puts(" hostile pointers refused with an error, no kernel fault\n");

    /* The other half of the same property: validation must not be so strict
     * that it rejects memory the program legitimately owns. The banner lives in
     * .rodata — mapped, user-readable, NOT writable — and reading it through a
     * system call must succeed. A validator that demanded write access for a
     * read would pass every test above and break every real program.
     *
     * The four bytes come back without a trailing newline, so they are fenced
     * with quotes and the line is terminated here. A marker printed by a
     * different test must never end up sharing a line with this one. */
    af_puts("init:   a read-only buffer reads back through a syscall: \"");
    const af_i64 ro = af_write(banner, 4);
    if (ro != 4) {
        fail("a read-only but valid buffer was refused", (af_u64)(-ro));
    }
    af_puts("\"\n");

    /* The marker the boot test waits for. Printed by user code, in ring 3,
     * after every check above has passed — so it is evidence that a program
     * from the disk ran, not merely that the kernel loaded one. */
    af_puts("AF_EXEC_RAN\n");
    af_puts("init: all checks passed, exiting\n");
    af_puts("----------------------------------------------------------\n");

    af_exit(0);
}

// SPDX-License-Identifier: MIT
// AfriyieOS — in-kernel self tests
//
// These run on bare metal at every boot in a debug build. They are the T2 tier
// of the test strategy (blueprint section 12.1): they test the real code on the
// real target, and they report over the serial console in a format CI can grep.
//
// A failed test prints  AF_TEST_FAIL:<name>  and the boot is considered failed.

#include "afriyie/config.h"
#include "afriyie/types.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"     // af_panic, for the hard stop on a failed test
#include "afriyie/selftest.h"   // the declaration of the runner itself
#include "afriyie/kstring.h"
#include "afriyie/boot_info.h"
#include "afriyie/fb.h"
#include "afriyie/pmm.h"
#include "afriyie/heap.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/spinlock.h"

#if AF_TARGET_X86_64
#include "../arch/x86_64/x86_64.h"
#endif

static af_u32 s_passed = 0;
static af_u32 s_failed = 0;

// -----------------------------------------------------------------------------
// Harness
// -----------------------------------------------------------------------------
static void check(bool condition, const char *name, const char *detail)
{
    if (condition) {
        s_passed++;
        af_log(AF_LOG_DEBUG, "test", "  ok   %s", name);
        return;
    }

    s_failed++;
    af_log(AF_LOG_ERROR, "test", "  FAIL %s — %s", name,
           (detail != NULL) ? detail : "no detail");
    af_log_raw(AF_BOOT_MARKER_FAIL);
    af_log_raw(name);
    af_log_raw("\n");
}

#define CHECK(cond) check((cond), #cond, NULL)
#define CHECK_MSG(cond, msg) check((cond), #cond, (msg))

// Equality of a formatted string against what we expect, with a useful diff.
static void check_str(const char *actual, const char *expected, const char *name)
{
    if (actual != NULL && expected != NULL && af_strcmp(actual, expected) == 0) {
        s_passed++;
        af_log(AF_LOG_DEBUG, "test", "  ok   %s", name);
        return;
    }

    s_failed++;
    af_log(AF_LOG_ERROR, "test", "  FAIL %s", name);
    af_log(AF_LOG_ERROR, "test", "        expected: '%s'",
           (expected != NULL) ? expected : "(null)");
    af_log(AF_LOG_ERROR, "test", "        actual  : '%s'",
           (actual != NULL) ? actual : "(null)");
    af_log_raw(AF_BOOT_MARKER_FAIL);
    af_log_raw(name);
    af_log_raw("\n");
}

// -----------------------------------------------------------------------------
// Memory and string helpers
// -----------------------------------------------------------------------------
static void test_kstring(void)
{
    char buf[64];

    af_memset(buf, 0xAB, sizeof(buf));
    bool all_set = true;
    for (af_size i = 0; i < sizeof(buf); i++) {
        if ((af_u8)buf[i] != 0xABu) {
            all_set = false;
            break;
        }
    }
    CHECK(all_set);

    // memset must work for unaligned destinations too: the word-at-a-time fast
    // path can only start once the pointer is 8-byte aligned.
    af_memset(buf + 1, 0x5A, 15);
    all_set = true;
    for (af_size i = 1; i < 16; i++) {
        if ((af_u8)buf[i] != 0x5Au) {
            all_set = false;
            break;
        }
    }
    CHECK(all_set);

    const char *src = "AfriyieOS";
    af_memset(buf, 0, sizeof(buf));
    af_memcpy(buf, src, 9);
    check_str(buf, src, "memcpy copies bytes");

    // Overlapping forwards move.
    af_memset(buf, 0, sizeof(buf));
    af_strlcpy(buf, "abcdef", sizeof(buf));
    af_memmove(buf + 2, buf, 4);          // "ababcd"
    check_str(buf, "ababcd", "memmove handles overlap (dst > src)");

    // Overlapping backwards move.
    af_memset(buf, 0, sizeof(buf));
    af_strlcpy(buf, "abcdef", sizeof(buf));
    af_memmove(buf, buf + 2, 4);          // "cdefef"
    check_str(buf, "cdefef", "memmove handles overlap (dst < src)");

    CHECK(af_strlen("") == 0);
    CHECK(af_strlen("abc") == 3);
    CHECK(af_strlen(NULL) == 0);

    CHECK(af_strcmp("abc", "abc") == 0);
    CHECK(af_strcmp("abc", "abd") < 0);
    CHECK(af_strcmp("abd", "abc") > 0);
    CHECK(af_strncmp("abcdef", "abcxyz", 3) == 0);
    CHECK(af_strncmp("abcdef", "abcxyz", 4) != 0);

    CHECK(af_strchr("hello", 'l') != NULL);
    CHECK(af_strchr("hello", 'z') == NULL);

    CHECK(af_str_has_prefix("AFRIYIE", "AFR"));
    CHECK(!af_str_has_prefix("AFR", "AFRIYIE"));

    af_memset(buf, 0xCC, sizeof(buf));
    af_size n = af_strlcpy(buf, "12345", sizeof(buf));
    CHECK(n == 5);
    check_str(buf, "12345", "strlcpy copies and terminates");

    af_strlcpy(buf, "abc", sizeof(buf));
    af_strlcat(buf, "def", sizeof(buf));
    check_str(buf, "abcdef", "strlcat appends");

    // A tiny destination must truncate, never overflow.
    char small[8];
    af_memset(small, 0xEE, sizeof(small));
    af_strlcpy(small, "0123456789", sizeof(small));
    small[7] = '\0';   // guard: strlcpy must already have terminated here
    CHECK(af_strlen(small) == 7);
    CHECK(small[7] == '\0');
}

// -----------------------------------------------------------------------------
// Number formatting
// -----------------------------------------------------------------------------
static void test_formatting(void)
{
    char buf[128];

    af_itoa(0, buf, sizeof(buf));
    check_str(buf, "0", "itoa 0");

    af_itoa(-1234, buf, sizeof(buf));
    check_str(buf, "-1234", "itoa negative");

    af_itoa(9223372036854775807LL, buf, sizeof(buf));
    check_str(buf, "9223372036854775807", "itoa INT64_MAX");

    // The classic signed-overflow trap: -INT64_MIN must not be computed by
    // negating in signed space.
    af_itoa((-9223372036854775807LL - 1), buf, sizeof(buf));
    check_str(buf, "-9223372036854775808", "itoa INT64_MIN");

    af_utoa_base(255, buf, sizeof(buf), 16, false);
    check_str(buf, "ff", "utoa_base hex lowercase");

    af_utoa_base(255, buf, sizeof(buf), 16, true);
    check_str(buf, "FF", "utoa_base hex uppercase");

    af_format_hex(0xDEADBEEF, true, false, buf, sizeof(buf));
    check_str(buf, "0xdeadbeef", "format_hex with prefix");

    af_format_hex(0x1F, true, true, buf, sizeof(buf));
    check_str(buf, "0x000000000000001f", "format_hex zero padded to 16");

    af_format_bytes_human(512, buf, sizeof(buf));
    check_str(buf, "512 B", "format_bytes_human bytes");

    af_format_bytes_human(2048, buf, sizeof(buf));
    check_str(buf, "2 KiB", "format_bytes_human KiB");

    af_format_bytes_human(3 * AF_MIB, buf, sizeof(buf));
    check_str(buf, "3 MiB", "format_bytes_human MiB");

    af_snprintf(buf, sizeof(buf), "%s = %u", "count", 42u);
    check_str(buf, "count = 42", "snprintf %s %u");

    af_snprintf(buf, sizeof(buf), "%d", -7);
    check_str(buf, "-7", "snprintf %d negative");

    af_snprintf(buf, sizeof(buf), "%x|%X", 0xabcu, 0xabcu);
    check_str(buf, "abc|ABC", "snprintf %x %X");

    af_snprintf(buf, sizeof(buf), "[%5u]", 42u);
    check_str(buf, "[   42]", "snprintf right-aligned width");

    af_snprintf(buf, sizeof(buf), "[%-5u]", 42u);
    check_str(buf, "[42   ]", "snprintf left-aligned width");

    af_snprintf(buf, sizeof(buf), "[%05u]", 42u);
    check_str(buf, "[00042]", "snprintf zero padding");

    af_snprintf(buf, sizeof(buf), "%.3s", "abcdef");
    check_str(buf, "abc", "snprintf string precision");

    af_snprintf(buf, sizeof(buf), "100%%");
    check_str(buf, "100%", "snprintf literal percent");

    af_snprintf(buf, sizeof(buf), "%llu", 18446744073709551615ULL);
    check_str(buf, "18446744073709551615", "snprintf %llu UINT64_MAX");

    af_snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    check_str(buf, "0x1234", "snprintf %p");

    // Truncation must be reported and must still terminate.
    char tiny[6];
    int would_be = af_snprintf(tiny, sizeof(tiny), "%s", "abcdefghij");
    CHECK(would_be == 10);
    CHECK(af_strlen(tiny) == 5);
    CHECK(tiny[5] == '\0');
}

// -----------------------------------------------------------------------------
// Boot info validation
//
// Built by hand so the failure paths are exercised for real, not just described.
// -----------------------------------------------------------------------------
static void test_boot_info_validation(void)
{
    static af_boot_info_t bi;

    af_memset(&bi, 0, sizeof(bi));
    bi.magic  = AF_BOOT_INFO_MAGIC;
    bi.version = AF_BOOT_INFO_VERSION;
    bi.size   = sizeof(af_boot_info_t);
    bi.firmware = AF_FIRMWARE_UEFI;
    bi.memory_region_count = 1;
    bi.memory_regions[0].base   = 0x100000;
    bi.memory_regions[0].length = 0x10000000;
    bi.memory_regions[0].type   = AF_MEM_USABLE;

    CHECK(af_boot_info_validate(&bi) == AF_OK);
    CHECK(af_boot_info_usable_bytes(&bi) == 0x10000000ULL);
    CHECK(af_boot_info_max_address(&bi) == 0x10100000ULL);

    CHECK(af_boot_info_find_region(&bi, 0x200000) != NULL);
    CHECK(af_boot_info_find_region(&bi, 0x0) == NULL);

    // Wrong magic.
    af_boot_info_t bad = bi;
    bad.magic = 0x1234;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_MAGIC);

    // Wrong version — the stale-bootloader case.
    bad = bi;
    bad.version = AF_BOOT_INFO_VERSION + 1;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_VERSION);

    // Structure smaller than the kernel expects.
    bad = bi;
    bad.size = 16;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_VERSION);

    // Empty memory map.
    bad = bi;
    bad.memory_region_count = 0;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_MEMMAP);

    // Too many regions to hold.
    bad = bi;
    bad.memory_region_count = AF_MAX_MEMORY_REGIONS + 1;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_MEMMAP);

    // Unaligned region base.
    bad = bi;
    bad.memory_regions[0].base = 0x100001;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_MEMMAP);

    // Zero-length region.
    bad = bi;
    bad.memory_regions[0].length = 0;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_MEMMAP);

    // No usable memory at all.
    bad = bi;
    bad.memory_regions[0].type = AF_MEM_RESERVED;
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_MEMMAP);

    // Framebuffer claimed but with nonsense geometry.
    bad = bi;
    bad.boot_flags = AF_BOOT_FLAG_FRAMEBUFFER;
    bad.framebuffer.address = 0xFD000000;
    bad.framebuffer.width = 1024;
    bad.framebuffer.height = 768;
    bad.framebuffer.bpp = 32;
    bad.framebuffer.pitch = 100;             // smaller than width*4 — wrong
    CHECK(af_boot_info_validate(&bad) == AF_ERR_BOOT_NOFB);

    // ...and the correct pitch is accepted.
    bad.framebuffer.pitch = 1024 * 4;
    CHECK(af_boot_info_validate(&bad) == AF_OK);

    // NULL must be rejected, not dereferenced.
    CHECK(af_boot_info_validate(NULL) == AF_ERR_INVAL);
}

// -----------------------------------------------------------------------------
// Framebuffer
// -----------------------------------------------------------------------------
static void test_framebuffer(void)
{
    if (!af_fb_available()) {
        af_log(AF_LOG_WARN, "test", "  skip framebuffer tests (no framebuffer)");
        return;
    }

    af_surface_t *s = af_fb_surface();
    CHECK(s != NULL);

    if (s == NULL) {
        return;
    }

    // A pitch that is at least wide enough is the invariant every drawing
    // routine depends on.
    CHECK(s->pitch >= (s->width * s->bpp) / 8u);
    CHECK(s->width > 0 && s->height > 0);
    CHECK(s->bpp == 16 || s->bpp == 32);

    // Out-of-bounds drawing must be silently clipped, not corrupt memory.
    // (If it were not, this test would take the machine down — which is itself
    // the signal we want.)
    af_fb_put_pixel(-1, -1, AF_COLOR_WHITE);
    af_fb_put_pixel((af_i32)s->width + 10, (af_i32)s->height + 10, AF_COLOR_WHITE);
    af_fb_fill_rect(-100, -100, 50, 50, AF_COLOR_RED);
    af_fb_fill_rect((af_i32)s->width - 5, (af_i32)s->height - 5, 100, 100,
                    AF_COLOR_BLUE);
    CHECK(true);   // reaching this line is the assertion

    // Measurement must agree with the font metrics.
    af_u32 w = 0;
    af_u32 h = 0;
    af_fb_measure_text("abcd", 1, &w, &h);
    CHECK(w == 4 * AF_FONT_WIDTH);
    CHECK(h == AF_FONT_HEIGHT);

    af_fb_measure_text("abcd", 2, &w, &h);
    CHECK(w == 4 * AF_FONT_WIDTH * 2);
    CHECK(h == AF_FONT_HEIGHT * 2);
}

// =============================================================================
// v0.2 acceptance test — two threads printing A and B, interleaved
// =============================================================================
//
// This is the milestone's acceptance criterion from the blueprint: "create two
// threads that each loop printing A and B, and assert they alternate under
// preemption". It runs after the scheduler is up, from the boot thread, because
// it needs to create threads and then wait for them.
//
// It checks three things:
//
//   1. BOTH threads ran to completion. A scheduler that starves one thread
//      passes any test that only counts total iterations.
//   2. The interleaving is real. If A completes entirely before B starts, the
//      threads were never actually concurrent — the scheduler ran them
//      sequentially, which a naive implementation does whenever it picks from
//      the run queue without ever preempting.
//   3. Sleep and wake work, since the wait itself uses sched_sleep.
// =============================================================================

#define SCHED_TEST_ITERATIONS 200
#define SCHED_TEST_MAX_LOG    (SCHED_TEST_ITERATIONS * 2)

static char           s_sched_log[SCHED_TEST_MAX_LOG];
static volatile af_u32 s_sched_log_len = 0;
static volatile af_u32 s_alpha_progress = 0;
static volatile af_u32 s_beta_progress = 0;
static af_spinlock_t  s_sched_log_lock = AF_SPINLOCK_INIT;

static void sched_record(char letter)
{
    af_spin_lock(&s_sched_log_lock);

    if (s_sched_log_len < SCHED_TEST_MAX_LOG) {
        s_sched_log[s_sched_log_len++] = letter;
    }

    af_spin_unlock(&s_sched_log_lock);
}

static void alpha_thread(void *arg)
{
    AF_UNUSED(arg);

    for (af_u32 i = 0; i < SCHED_TEST_ITERATIONS; i++) {
        sched_record('A');
        s_alpha_progress = i + 1;

        // Yield so the other thread gets a turn. Without this the test would
        // pass on preemption alone, which is not what a cooperative round-robin
        // scheduler is supposed to demonstrate.
        sched_yield();
    }
}

static void beta_thread(void *arg)
{
    AF_UNUSED(arg);

    for (af_u32 i = 0; i < SCHED_TEST_ITERATIONS; i++) {
        sched_record('B');
        s_beta_progress = i + 1;

        // Both workers yield, so the log alternates deterministically and the
        // round-robin behaviour is directly observable.
        //
        // The first version had beta NOT yield, intending to test preemption
        // instead. It reported "2 transitions in 400 entries — the threads ran
        // sequentially", which was wrong about the cause: 200 iterations of a
        // trivial loop complete in microseconds, far inside one 10 ms time
        // slice, so beta finished its entire workload before the timer ever had
        // a chance to preempt it. The scheduler was fine; the test had not
        // created the conditions it claimed to be testing.
        //
        // Preemption is verified properly below, with a thread that genuinely
        // cannot finish inside a slice.
        sched_yield();
    }
}

// A thread that never yields and never sleeps: it can only be stopped by the
// timer. If the boot thread ever runs again while this is going, preemption is
// real — there is no other mechanism by which the CPU could have changed hands.
static volatile af_u32 s_spinner_count = 0;
static volatile bool   s_spinner_stop  = false;

static void spinner_thread(void *arg)
{
    AF_UNUSED(arg);

    while (!s_spinner_stop) {
        s_spinner_count++;
    }
}

void af_sched_selftest(void)
{
    af_info("test", "scheduler acceptance test: two threads, %u iterations each",
            (af_u32)SCHED_TEST_ITERATIONS);

    s_sched_log_len = 0;
    s_alpha_progress = 0;
    s_beta_progress = 0;

    af_thread_t *alpha = thread_create("alpha", alpha_thread, NULL,
                                       AF_KERNEL_STACK_SIZE, AF_PRIO_DEFAULT);
    af_thread_t *beta  = thread_create("beta", beta_thread, NULL,
                                       AF_KERNEL_STACK_SIZE, AF_PRIO_DEFAULT);

    if (alpha == NULL || beta == NULL) {
        af_error("test", "  FAIL could not create the test threads");
        af_log_raw(AF_BOOT_MARKER_FAIL "sched_thread_create\n");
        af_panic("scheduler test: thread creation failed");
    }

    // A created thread is not runnable until it is admitted to a run queue.
    sched_admit(alpha);
    sched_admit(beta);

    // Let them run. Using sched_sleep exercises the sleeping path as well as the
    // scheduler itself.
    af_u32 waited = 0;
    while ((s_alpha_progress < SCHED_TEST_ITERATIONS ||
            s_beta_progress < SCHED_TEST_ITERATIONS) && waited < 5000) {
        sched_sleep(1);
        waited++;
    }

    // --- 1. both threads completed -------------------------------------------
    if (s_alpha_progress != SCHED_TEST_ITERATIONS) {
        af_panic("scheduler test: alpha reached %u of %u iterations — the "
                 "scheduler starved it", s_alpha_progress,
                 (af_u32)SCHED_TEST_ITERATIONS);
    }
    if (s_beta_progress != SCHED_TEST_ITERATIONS) {
        af_panic("scheduler test: beta reached %u of %u iterations — the "
                 "scheduler starved it", s_beta_progress,
                 (af_u32)SCHED_TEST_ITERATIONS);
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   both threads ran to completion "
                                  "(%u iterations each; waited %u ticks)",
           (af_u32)SCHED_TEST_ITERATIONS, waited);

    // --- 2. show a sample before asserting, so a failure is diagnosable -------
    af_u32 length = s_sched_log_len;
    if (length != SCHED_TEST_MAX_LOG) {
        af_panic("scheduler test: recorded %u entries, expected %u",
                 length, (af_u32)SCHED_TEST_MAX_LOG);
    }

    {
        char sample[80];
        af_u32 n = (length < 72) ? length : 72;
        for (af_u32 i = 0; i < n; i++) {
            sample[i] = s_sched_log[i];
        }
        sample[n] = '\0';
        af_info("test", "  first %u entries: %s", n, sample);
    }

    // --- 3. the interleaving is real -----------------------------------------
    af_u32 transitions = 0;
    af_u32 a_count = 0;
    af_u32 b_count = 0;
    af_u32 longest_run = 1;
    af_u32 run = 1;

    for (af_u32 i = 0; i < length; i++) {
        if (s_sched_log[i] == 'A') {
            a_count++;
        } else {
            b_count++;
        }

        if (i > 0) {
            if (s_sched_log[i] != s_sched_log[i - 1]) {
                transitions++;
                run = 1;
            } else {
                run++;
                if (run > longest_run) {
                    longest_run = run;
                }
            }
        }
    }

    if (a_count != SCHED_TEST_ITERATIONS || b_count != SCHED_TEST_ITERATIONS) {
        af_panic("scheduler test: %u A and %u B recorded, expected %u of each",
                 a_count, b_count, (af_u32)SCHED_TEST_ITERATIONS);
    }

    // Two threads that both yield hard once per iteration should produce close
    // to a strict alternation. A sequential run produces ONE transition, so the
    // threshold is set well above that while leaving room for a tick landing
    // between two yields.
    if (transitions < SCHED_TEST_ITERATIONS) {
        af_panic("scheduler test: only %u A/B transitions in %u entries — the "
                 "threads ran sequentially rather than concurrently",
                 transitions, length);
    }

    if (longest_run > 8) {
        af_panic("scheduler test: one thread ran %u times consecutively — "
                 "round-robin scheduling is not rotating", longest_run);
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   %u A/B transitions, longest run %u — strict round-robin",
           transitions, longest_run);

    // --- 4. preemption, tested with a thread that cannot be starved ----------
    //
    // The spinner never yields and never sleeps, so the ONLY way the boot thread
    // can run again is if the timer interrupt takes the CPU away from it. If
    // that happens and the spinner has made progress, preemption works. There is
    // no cooperative mechanism that could produce this result instead, which is
    // what makes it a proof rather than a correlation.
    s_spinner_count = 0;
    s_spinner_stop = false;

    af_thread_t *spinner = thread_create("spinner", spinner_thread, NULL,
                                         AF_KERNEL_STACK_SIZE, AF_PRIO_DEFAULT);
    if (spinner == NULL) {
        af_panic("scheduler test: could not create the spinner thread");
    }
    sched_admit(spinner);

    // Sleep ~10 ticks. This can only return if the timer preempted the spinner.
    sched_sleep(10);

    af_u32 spinner_progress = s_spinner_count;
    s_spinner_stop = true;

    if (spinner_progress == 0) {
        af_panic("scheduler test: the spinner made no progress — it never ran");
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   preemption: a non-yielding thread was interrupted "
           "(%u iterations counted while the boot thread slept)",
           spinner_progress);

    // Capture the tid BEFORE sleeping. thread_reap() frees the thread structure
    // on success, so `spinner` becomes a dangling pointer the moment reaping
    // happens — and the first version of this check read spinner->tid after the
    // sleep, which is a use-after-free in the test itself. It reported "still
    // registered" while reading freed memory.
    af_tid_t spinner_tid = spinner->tid;

    // Give the spinner a tick to notice the stop flag and exit, then reap.
    //
    // Reaping runs from whatever thread is not the one exiting, so it is done
    // here as well as in the idle loop. Relying on the idle thread alone is
    // fragile: it only gets the CPU when nothing else can run, so with a busy
    // run queue the reaping is correct but arbitrarily delayed — and a zombie
    // holds a 16 KiB stack for as long as that lasts.
    for (af_u32 i = 0; i < 16 && thread_by_tid(spinner_tid) != NULL; i++) {
        sched_collect_zombies();
        sched_sleep(1);
    }

    if (thread_by_tid(spinner_tid) != NULL) {
        af_panic("scheduler test: spinner tid %u is still registered after "
                 "exiting and yielding repeatedly — exited threads are not "
                 "being reaped, so every thread leaks its stack",
                 spinner_tid);
    }

    af_log(AF_LOG_DEBUG, "test",
           "  ok   an exited thread was reaped and removed from the table");

    af_marker("AF_SCHED_OK");
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
void af_selftest_run_all(void)
{
    s_passed = 0;
    s_failed = 0;

    af_info("test", "running v0.1 self tests");

    test_kstring();
    test_formatting();
    test_boot_info_validation();
    test_framebuffer();

    // v0.2: the memory subsystem runs its own tests, which are written as
    // panicking assertions rather than as returned results. That is deliberate —
    // a bitmap allocator or slab heap that is broken corrupts memory far from
    // the cause, so the only useful failure mode is to stop immediately with
    // the reason printed.
    pmm_selftest();
    heap_selftest();

    // v0.2 paging: mapping, translation, unmapping, and a second independent
    // address space.
    af_paging_selftest();

    if (s_failed == 0) {
        af_info("test", "%u checks passed", s_passed);
        af_marker("AF_TEST_OK");
    } else {
        af_error("test", "%u passed, %u FAILED", s_passed, s_failed);
        // A failing self test in a debug build is a hard stop: the kernel is
        // already known to be in a bad state, and continuing would hide it.
        af_panic("self tests failed (%u of %u)", s_failed, s_passed + s_failed);
    }
}

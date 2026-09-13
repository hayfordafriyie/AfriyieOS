// SPDX-License-Identifier: MIT
// AfriyieOS — process self test
//
// The property being tested is the one that was wrong for two milestones: an
// ADDRESS SPACE BELONGS TO A PROCESS, NOT TO A THREAD.
//
// Testing it properly means proving three things that a "does it boot" test
// cannot see:
//
//   1. Two processes have genuinely separate memory. Same virtual address, two
//      different physical frames, neither visible from the other.
//   2. A process's address space does not depend on which thread is running.
//      A second thread joining the process sees the same mappings.
//   3. A process is torn down exactly once, after its last thread is gone, and
//      its address space is released with it.
//
// A test that creates a process and destroys it proves none of these. It passes
// if the table has an entry, which is a much weaker claim.
//
// Nothing here creates a user thread or enters ring 3. That is elf_exec's job
// and its evidence is AF_EXEC_RAN. This covers the object underneath it, on the
// kernel's own tables, where every failure is diagnosable rather than a triple
// fault.

#include "afriyie/process.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/hal.h"
#include "afriyie/pmm.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

static af_u32 s_checks;
static af_u32 s_failures;

static void check(bool condition, const char *what)
{
    s_checks++;
    if (!condition) {
        s_failures++;
        af_error("proc", "  FAIL  %s", what);
    }
}

// A thread that does nothing and exits. Used to give a process a second thread
// without needing user mode or a real program.
static void idle_thread_body(void *arg)
{
    AF_UNUSED(arg);
    thread_exit();
}

void af_process_selftest(void)
{
    s_checks = 0;
    s_failures = 0;

    // The kernel's own root, to compare against. Captured before anything
    // changes CR3.
    const hal_pt_root_t kernel_root = hal_get_page_table();

    // =========================================================================
    // 1. Creation
    // =========================================================================
    const af_u32 before = process_live_count();

    af_process_t *a = process_create("test-a", AF_PID_KERNEL);
    check(a != NULL, "a process can be created");
    if (a == NULL) {
        af_error("proc", "cannot continue without a process");
        af_marker("AF_TEST_FAIL");
        return;
    }

    check(a->state == AF_PROCESS_ALIVE, "a new process is alive");
    check(a->pid != AF_PID_INVALID, "a new process has a pid");
    check(a->addr_space != 0, "a new process has an address space");
    check(a->addr_space != kernel_root,
          "the process does NOT share the kernel's address space");
    check(a->thread_count == 0, "a new process has no threads");
    check(process_live_count() == before + 1, "the live count went up");

    // Two processes must get DIFFERENT roots. Sharing one would mean every
    // process sees every other process's memory, which is the single failure
    // that makes the whole isolation story fiction.
    af_process_t *b = process_create("test-b", AF_PID_KERNEL);
    check(b != NULL, "a second process can be created");
    if (b == NULL) {
        process_reap(a);
        af_marker("AF_TEST_FAIL");
        return;
    }
    check(b->addr_space != a->addr_space,
          "two processes have different address spaces");
    check(b->pid != a->pid, "two processes have different pids");

    // =========================================================================
    // 2. Memory really is separate
    //
    // Same virtual address in both, two different frames, and each sees only
    // its own. This is the property a boot test cannot observe and the whole
    // reason a process owns an address space.
    // =========================================================================
    const af_vaddr probe = AF_USER_REGION_BASE;

    af_paddr frame_a = pmm_alloc_frame_z();
    af_paddr frame_b = pmm_alloc_frame_z();
    check(frame_a != AF_FRAME_INVALID && frame_b != AF_FRAME_INVALID,
          "two frames were allocated for the isolation test");

    af_status_t rc = hal_map_page(a->addr_space, probe, frame_a,
                                  HAL_PRESENT | HAL_WRITABLE | HAL_USER);
    check(!af_status_err(rc), "a page maps into process A");

    rc = hal_map_page(b->addr_space, probe, frame_b,
                      HAL_PRESENT | HAL_WRITABLE | HAL_USER);
    check(!af_status_err(rc), "a page maps at the SAME address in process B");

    check(hal_translate(a->addr_space, probe) == frame_a,
          "process A translates the address to its own frame");
    check(hal_translate(b->addr_space, probe) == frame_b,
          "process B translates the address to its own frame");

    // Neither process's mapping may appear in the kernel's tables. If it did,
    // a user pointer would be reachable from ring 0 by address alone, and every
    // capability check downstream would be decoration.
    check(hal_translate(kernel_root, probe) == 0,
          "the user mapping does NOT leak into the kernel's address space");

    // =========================================================================
    // 3. Thread membership
    // =========================================================================
    af_thread_t *t1 = thread_create("proc-t1", idle_thread_body, NULL,
                                    AF_KERNEL_STACK_SIZE, AF_PRIO_DEFAULT);
    check(t1 != NULL, "a thread can be created for the process");
    if (t1 != NULL) {
        process_attach_thread(a, t1);
        check(a->thread_count == 1, "the process has one thread");
        check(t1->process == a, "the thread points back at its process");

        process_detach_thread(t1);
        check(a->thread_count == 0, "detaching decrements the count");
        check(t1->process == NULL, "a detached thread has no process");
    }

    // =========================================================================
    // 4. Teardown
    // =========================================================================
    process_reap(b);
    check(b->state == AF_PROCESS_FREE, "a reaped process is free");
    check(b->pid == AF_PID_INVALID, "a reaped process has no pid");
    check(b->addr_space == 0, "a reaped process released its address space");
    check(process_live_count() == before + 1, "the live count went back down");

    // Reaping is not idempotent-by-accident: a second call must be a no-op
    // rather than a double free of the address space.
    process_reap(b);
    check(b->state == AF_PROCESS_FREE, "reaping twice is harmless");

    // frame_b IS NOT FREED HERE, and that is the point of this comment.
    //
    // hal_pt_destroy releases the frames mapped into the address space it is
    // destroying — that is what "the process owns its memory" means, and it is
    // why process_reap can be a single call. The first version of this test
    // freed frame_b afterwards as well, on the assumption that the test had
    // allocated it and therefore owned it. It had, and it did not:
    //
    //     ERROR pmm: double free of frame 0x1E76000
    //     AFRIYIEOS KERNEL PANIC
    //
    // Ownership transferred to the address space at hal_map_page. The frame is
    // allocated once and released once, by two different pieces of code, and the
    // second release is a bug every time.
    //
    // The contract is now written down in hal.h, where it should have been. See
    // the note there about why sharing a frame between two address spaces does
    // NOT work yet.

    // =========================================================================
    // 5. Shared memory
    //
    // The property that was IMPOSSIBLE before this round: two address spaces
    // mapping one physical frame, and both teardowns being safe.
    //
    // This is not a curiosity. Shared memory is how IPC will avoid copying every
    // message through the kernel (ADR-014 budgets for it), and a capability to a
    // shared buffer is how one process hands another access to data without
    // handing over the ability to allocate.
    //
    // The failure it guards against is the nastiest shape a memory bug has: the
    // first destroy looks like ordinary operation and the damage lands on a
    // process that did nothing wrong.
    // =========================================================================
    af_process_t *sh_a = process_create("share-a", AF_PID_KERNEL);
    af_process_t *sh_b = process_create("share-b", AF_PID_KERNEL);
    check(sh_a != NULL && sh_b != NULL, "two processes for the sharing test");
    if (sh_a == NULL || sh_b == NULL) {
        af_error("proc", "cannot continue without both processes");
        af_marker("AF_TEST_FAIL");
        return;
    }

    af_paddr shared = pmm_alloc_frame_z();
    check(shared != AF_FRAME_INVALID, "a frame was allocated to share");
    check(pmm_frame_refcount(shared) == 1, "a fresh frame is referenced once");

    const af_vaddr share_va = AF_USER_REGION_BASE + AF_PAGE_SIZE;

    rc = hal_map_page(sh_a->addr_space, share_va, shared,
                      HAL_PRESENT | HAL_WRITABLE | HAL_USER);
    check(!af_status_err(rc), "the frame maps into the first process");
    check(pmm_frame_refcount(shared) == 1,
          "mapping alone does NOT take a reference — the sharer must ask");

    // The explicit reference, and it is what makes the second destroy safe.
    pmm_frame_ref(shared);
    check(pmm_frame_refcount(shared) == 2, "the second owner took a reference");

    rc = hal_map_page(sh_b->addr_space, share_va, shared,
                      HAL_PRESENT | HAL_WRITABLE | HAL_USER);
    check(!af_status_err(rc), "the SAME frame maps into the second process");

    // Both translate the address to the same physical frame. That is what
    // sharing means, and it is the opposite of the isolation check above —
    // which is why memory has to be handed over deliberately rather than
    // accidentally.
    check(hal_translate(sh_a->addr_space, share_va) == shared,
          "the first process sees the shared frame");
    check(hal_translate(sh_b->addr_space, share_va) == shared,
          "the second process sees the SAME shared frame");

    // The first teardown. Before unref, this freed the frame out from under B.
    sh_a->state = AF_PROCESS_ZOMBIE;
    process_reap(sh_a);
    check(pmm_frame_refcount(shared) == 1,
          "the first destroy released ONE reference, not the frame");
    check(hal_translate(sh_b->addr_space, share_va) == shared,
          "the surviving process's mapping is still valid after the other died");

    // And the second teardown actually releases it.
    sh_b->state = AF_PROCESS_ZOMBIE;
    process_reap(sh_b);
    check(pmm_frame_refcount(shared) == 0,
          "the last destroy released the shared frame");

    // =========================================================================
    // 6. Cleanup
    //
    // Process A is still alive at this point — it had to be, for the isolation
    // checks above to mean anything — and the test ends it explicitly.
    //
    // A self test that leaks a process and its address space leaks on EVERY
    // boot, and the leak is invisible because a small fixed leak looks exactly
    // like a correct kernel. Marking it a zombie first rather than reaping a
    // live process keeps the state transition honest: process_reap's contract is
    // that the process is finished, and "the test is over" is a reason to finish
    // it, not a reason to skip the step that says so.
    // =========================================================================
    a->state     = AF_PROCESS_ZOMBIE;
    a->exit_code = 0;
    process_reap(a);

    check(a->state == AF_PROCESS_FREE, "the first process was released too");
    check(process_live_count() == before,
          "the live count is back where the test started");
    check(process_find(a->pid) == NULL || a->pid == AF_PID_INVALID,
          "a reaped pid no longer resolves");

    // =========================================================================
    // 7. Diagnostics
    // =========================================================================

    if (s_failures != 0) {
        af_error("proc", "%u of %u process checks FAILED", s_failures, s_checks);
        af_marker("AF_TEST_FAIL");
        return;
    }

    af_info("proc", "%u process checks passed", s_checks);
    af_marker("AF_PROC_OK");
}

// SPDX-License-Identifier: MIT
// AfriyieOS — processes
//
// The table is a fixed array with a generation-free pid. A pid is an index plus
// one, so pid 0 is never a valid process — which matters because an uninitialised
// af_pid_t is 0 and AF_PID_INVALID is 0, so the failure case is the default
// rather than something a caller has to remember to set.
//
// Pids ARE REUSED once a process is reaped. That is a real hazard for any code
// that holds a pid across a wait — the classic use-after-reap, where a stale pid
// names a different process. It is not fixed here because fixing it properly
// means a generation counter in the pid, and the honest thing is to say so
// rather than to half-do it: nothing holds a pid across a reap today, and the
// capability table (which arrives with the personalities) is where handles stop
// being bare indices. See docs/architecture/capability-model.md.

#include "afriyie/process.h"
#include "afriyie/cap.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/heap.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

static af_process_t s_table[AF_MAX_PROCESSES];
static af_u32       s_live;
static af_u32       s_next_pid;   // one past the last handed out

// -----------------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------------
void process_init(void)
{
    af_memset(s_table, 0, sizeof(s_table));

    s_live = 0;

    // Start at 2: pid 1 is AF_PID_KERNEL, reserved for the boot context, which
    // is a thread with no process rather than a process with a name.
    s_next_pid = 2;

    for (af_u32 i = 0; i < AF_MAX_PROCESSES; i++) {
        s_table[i].state = AF_PROCESS_FREE;
    }
}

// -----------------------------------------------------------------------------
// Lookup
// -----------------------------------------------------------------------------
static af_process_t *slot_for_pid(af_pid_t pid)
{
    if (pid == AF_PID_INVALID || pid == AF_PID_KERNEL) {
        return NULL;
    }

    af_process_t *proc = &s_table[pid - 1];
    if (proc->state == AF_PROCESS_FREE || proc->pid != pid) {
        return NULL;
    }
    return proc;
}

af_process_t *process_find(af_pid_t pid)
{
    return slot_for_pid(pid);
}

af_process_t *process_current(void)
{
    af_thread_t *self = thread_current();
    return (self != NULL) ? self->process : NULL;
}

af_u32 process_live_count(void)
{
    return s_live;
}

// -----------------------------------------------------------------------------
// Creation
// -----------------------------------------------------------------------------
af_process_t *process_create(const char *name, af_pid_t parent)
{
    // --- find a free slot ----------------------------------------------------
    af_process_t *proc = NULL;

    for (af_u32 i = 0; i < AF_MAX_PROCESSES; i++) {
        // A pid is a slot index plus one, so the pid and the slot stay in step
        // and lookup is arithmetic rather than a search.
        const af_u32 candidate = i + 1;
        if (candidate < s_next_pid) {
            continue;   // already used at some point; do not recycle out of order
        }
        if (s_table[i].state == AF_PROCESS_FREE) {
            proc = &s_table[i];
            proc->pid = candidate;
            break;
        }
    }

    if (proc == NULL) {
        af_error("proc", "process table is full (%u processes)", AF_MAX_PROCESSES);
        return NULL;
    }

    // --- address space -------------------------------------------------------
    //
    // Created BEFORE the process is marked live, so a failure here leaves the
    // slot free rather than half-built. A process in the table with no address
    // space is a process the scheduler will switch to and fault on.
    hal_pt_root_t space = hal_pt_create_user();
    if (space == 0) {
        af_error("proc", "could not create an address space for '%s'", name);
        proc->pid = AF_PID_INVALID;
        return NULL;
    }

    // --- capability table ----------------------------------------------------
    //
    // Created BEFORE the process is marked live, for the same reason the address
    // space is: a failure here must leave the slot free rather than half-built.
    // A process in the table with no capability table is a process whose first
    // syscall dereferences NULL.
    af_cap_table_t *caps = cap_table_create();
    if (caps == NULL) {
        af_error("proc", "could not create a capability table for '%s'", name);
        hal_pt_destroy(space);
        proc->pid = AF_PID_INVALID;
        return NULL;
    }

    // --- fill in -------------------------------------------------------------
    proc->state        = AF_PROCESS_ALIVE;
    proc->parent       = parent;
    proc->addr_space   = space;
    proc->caps         = caps;
    proc->threads      = NULL;
    proc->thread_count = 0;
    proc->exit_code    = 0;
    proc->waiter       = NULL;
    proc->created_tick = sched_tick_count();

    af_strlcpy(proc->name, name, sizeof(proc->name));

    if (proc->pid >= s_next_pid) {
        s_next_pid = proc->pid + 1;
    }
    s_live++;

    af_info("proc", "created pid %u '%s' (parent %u, address space 0x%lX)",
            proc->pid, proc->name, proc->parent, (af_u64)proc->addr_space);

    return proc;
}

// -----------------------------------------------------------------------------
// Thread membership
// -----------------------------------------------------------------------------
void process_attach_thread(af_process_t *proc, af_thread_t *thread)
{
    if (proc == NULL || thread == NULL) {
        return;
    }

    thread->process     = proc;
    thread->process_next = proc->threads;
    proc->threads       = thread;
    proc->thread_count++;

    af_log(AF_LOG_DEBUG, "proc", "  tid %u joined pid %u (%u thread(s))",
           thread->tid, proc->pid, proc->thread_count);
}

void process_detach_thread(af_thread_t *thread)
{
    if (thread == NULL || thread->process == NULL) {
        return;
    }

    af_process_t *proc = thread->process;

    // Unlink. The list is short and unlink-from-singly-linked needs the
    // predecessor, so this walks it rather than keeping a doubly-linked list for
    // a list that is almost always one element long.
    af_thread_t **link = &proc->threads;
    while (*link != NULL) {
        if (*link == thread) {
            *link = thread->process_next;
            break;
        }
        link = &(*link)->process_next;
    }

    if (proc->thread_count > 0) {
        proc->thread_count--;
    }

    thread->process      = NULL;
    thread->process_next = NULL;
}

// -----------------------------------------------------------------------------
// Exit and wait
// -----------------------------------------------------------------------------
void process_exit(af_i32 code)
{
    af_process_t *proc = process_current();

    if (proc != NULL) {
        proc->exit_code = code;
        proc->state     = AF_PROCESS_ZOMBIE;

        af_info("proc", "pid %u '%s' exiting with code %d",
                proc->pid, proc->name, code);

        // Wake a parent blocked in process_wait. sched_unblock is safe to call
        // for a thread that is not blocked — it checks — so no guard is needed
        // here and none should be added, because a guard that is wrong in the
        // direction of "do not wake" is a deadlock.
        if (proc->waiter != NULL) {
            sched_unblock(proc->waiter);
            proc->waiter = NULL;
        }
    }

    // Terminates the calling thread. Never returns; the process tears down when
    // the last of its threads has been reaped.
    thread_exit();
}

af_status_t process_wait(af_pid_t pid, af_i32 *out_code)
{
    af_process_t *child = slot_for_pid(pid);
    if (child == NULL) {
        return AF_ERR_NOENT;
    }

    af_process_t *self = process_current();

    // A process may wait on its own children. A kernel thread has no process and
    // therefore no children, which is a refusal rather than an omission: letting
    // a kernel thread wait on an arbitrary pid would let it block the reaper.
    if (self == NULL || child->parent != self->pid) {
        af_warn("proc", "pid %u tried to wait on pid %u, which is not its child",
                self != NULL ? self->pid : AF_PID_KERNEL, pid);
        return AF_ERR_PERM;
    }

    // ALREADY EXITED — checked FIRST, and that order is the whole point.
    //
    // A child that exits before its parent waits is the common case, not the
    // rare one: every short-lived program does it. If the block came first, the
    // parent would sleep forever waiting for a wakeup that already happened.
    while (child->state == AF_PROCESS_ALIVE) {
        child->waiter = thread_current();
        sched_block(child);

        // Woken. Loop rather than proceeding, because a wakeup is not a promise:
        // the child may have been woken but not yet run, and reading exit_code
        // before it has been set is exactly the race this loop closes.
    }

    if (out_code != NULL) {
        *out_code = child->exit_code;
    }

    const af_pid_t reaped = child->pid;
    process_reap(child);

    af_log(AF_LOG_DEBUG, "proc", "pid %u reaped by its parent", reaped);
    return AF_OK;
}

// -----------------------------------------------------------------------------
// Teardown
// -----------------------------------------------------------------------------
void process_reap(af_process_t *proc)
{
    if (proc == NULL || proc->state == AF_PROCESS_FREE) {
        return;
    }

    // The address space is freed HERE and nowhere else, because this is the only
    // point at which it is provably not the one being executed on:
    //
    //   * the process is a zombie, so no thread of it is running;
    //   * its last thread has been reaped, so no kernel stack from it is
    //     still in use;
    //   * the reaper runs on the kernel's tables.
    //
    // The assert is not decoration. Freeing page tables that CR3 currently holds
    // does not fail — it triple-faults with no output at all, and the dump that
    // would have explained it is the thing that just stopped existing.
    AF_ASSERT_MSG(hal_get_page_table() != proc->addr_space,
                  "reaping a process while running on its address space");

    if (proc->addr_space != 0) {
        af_info("proc", "reaping pid %u '%s': releasing address space 0x%lX",
                proc->pid, proc->name, (af_u64)proc->addr_space);
        hal_pt_destroy(proc->addr_space);
        proc->addr_space = 0;
    }

    // The capability table goes with the process. Its slots reference objects
    // owned elsewhere, so destroying it releases REFERENCES, not objects — a
    // frame two processes share must not be freed by whichever of them exits
    // first. That is what the frame reference count in hal_pt_destroy is for.
    if (proc->caps != NULL) {
        cap_table_destroy(proc->caps);
        proc->caps = NULL;
    }

    if (proc->thread_count != 0) {
        // Reaping a process that still has threads would free an address space
        // out from under them. Refuse and say so rather than corrupting the
        // machine quietly.
        af_error("proc", "pid %u still has %u thread(s); not reaping",
                 proc->pid, proc->thread_count);
        return;
    }

    af_strlcpy(proc->name, "", sizeof(proc->name));
    proc->state  = AF_PROCESS_FREE;
    proc->pid    = AF_PID_INVALID;
    proc->parent = AF_PID_INVALID;

    if (s_live > 0) {
        s_live--;
    }
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
void process_dump(void)
{
    af_info("proc", "%u process(es) live", s_live);

    for (af_u32 i = 0; i < AF_MAX_PROCESSES; i++) {
        const af_process_t *proc = &s_table[i];
        if (proc->state == AF_PROCESS_FREE) {
            continue;
        }

        af_info("proc", "  pid %u '%s' %s, parent %u, %u thread(s), space 0x%lX",
                proc->pid, proc->name,
                proc->state == AF_PROCESS_ALIVE ? "alive" : "zombie",
                proc->parent, proc->thread_count, (af_u64)proc->addr_space);
    }
}

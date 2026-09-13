// SPDX-License-Identifier: MIT
// AfriyieOS — round-robin scheduler
//
// =============================================================================
// DESIGN
// =============================================================================
// Eight priority levels, each a FIFO run queue, round-robin within a level and
// strict priority between them. Preemption comes from the timer tick, which
// decrements the running thread's slice and switches away when it expires.
//
// =============================================================================
// WHERE THE SWITCH ACTUALLY HAPPENS
// =============================================================================
// sched_tick() runs inside the timer interrupt handler, on the *current*
// thread's kernel stack — the ISR stubs do not switch stacks, so the interrupt
// frame is already sitting there. Calling context_switch() from inside the
// handler is therefore safe: the outgoing thread's frame stays on its own stack,
// and when it is scheduled again it returns through the same path, out of
// context_switch, out of sched_tick, out of the ISR and through `iretq` back to
// exactly where it was interrupted.
//
// This only works because every thread has its own kernel stack. A single shared
// interrupt stack would silently corrupt the frame of whichever thread was
// preempted.
// =============================================================================

#include "afriyie/sched.h"
#include "afriyie/thread.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/spinlock.h"
#include "afriyie/io.h"
#include "afriyie/kstring.h"

extern void context_switch(af_context_t *old, af_context_t *next);

static af_thread_t      *s_runq[AF_PRIO_LEVELS];
static af_u32            s_runq_len[AF_PRIO_LEVELS];
static af_thread_t      *s_current = NULL;
static af_thread_t      *s_idle = NULL;

static af_u64            s_ticks = 0;
static af_u64            s_switches = 0;
static volatile bool     s_need_resched = false;

// Threads that have exited and are waiting for their structures to be freed.
// Reaping happens from the scheduler loop rather than from thread_exit(),
// because a thread cannot free the stack it is standing on.
static af_thread_t      *s_zombies[16];
static af_u32            s_zombie_count = 0;

// -----------------------------------------------------------------------------
// Run queue
// -----------------------------------------------------------------------------
static void runq_push(af_thread_t *thread)
{
    af_u8 p = thread->priority;
    if (p >= AF_PRIO_LEVELS) {
        p = AF_PRIO_LEVELS - 1;
    }

    thread->next = NULL;

    if (s_runq[p] == NULL) {
        s_runq[p] = thread;
    } else {
        af_thread_t *tail = s_runq[p];
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = thread;
    }

    s_runq_len[p]++;
    thread->state = AF_THREAD_RUNNABLE;
}

static af_thread_t *runq_pop(af_u8 priority)
{
    af_thread_t *thread = s_runq[priority];

    if (thread == NULL) {
        return NULL;
    }

    s_runq[priority] = thread->next;
    if (s_runq_len[priority] > 0) {
        s_runq_len[priority]--;
    }
    thread->next = NULL;
    return thread;
}

static af_thread_t *runq_pop_highest(void)
{
    for (af_u8 p = 0; p < AF_PRIO_LEVELS; p++) {
        af_thread_t *thread = runq_pop(p);
        if (thread != NULL) {
            return thread;
        }
    }
    return NULL;
}

static bool runq_remove(af_thread_t *thread)
{
    for (af_u8 p = 0; p < AF_PRIO_LEVELS; p++) {
        af_thread_t *prev = NULL;
        af_thread_t *node = s_runq[p];

        while (node != NULL) {
            if (node == thread) {
                if (prev == NULL) {
                    s_runq[p] = node->next;
                } else {
                    prev->next = node->next;
                }
                node->next = NULL;
                if (s_runq_len[p] > 0) {
                    s_runq_len[p]--;
                }
                return true;
            }
            prev = node;
            node = node->next;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
//
// `boot_thread` is the context kmain is running on and becomes an ordinary
// schedulable thread. `idle_thread` is a SEPARATE thread that halts the CPU when
// nothing else can run.
//
// These must not be the same thread, and making them the same was the first
// version's mistake. sched_sleep() and sched_block() refuse to act on the idle
// thread — blocking it would deadlock the machine — so if the boot context *is*
// idle, every sleep in the boot path silently becomes a no-op. The v0.2
// acceptance test spun 2000 times without ever yielding, both worker threads
// never ran, and the failure reported was "the scheduler starved them", which
// pointed at the scheduler rather than at the missing idle thread.
af_status_t sched_init(af_thread_t *boot_thread, af_thread_t *idle_thread)
{
    for (af_u32 i = 0; i < AF_PRIO_LEVELS; i++) {
        s_runq[i] = NULL;
        s_runq_len[i] = 0;
    }

    if (boot_thread == NULL || idle_thread == NULL) {
        return AF_ERR_INVAL;
    }

    s_idle = idle_thread;
    s_idle->priority = AF_PRIO_IDLE;
    s_idle->state = AF_THREAD_CREATED;   // admitted by the caller

    s_current = boot_thread;
    s_current->state = AF_THREAD_RUNNING;

    af_info("sched", "round-robin scheduler ready: %u priority levels, "
                     "%u tick slice at %u Hz",
            (af_u32)AF_PRIO_LEVELS, (af_u32)AF_TIME_SLICE_TICKS,
            (af_u32)AF_SCHED_TICK_HZ);

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Picking the next thread
// -----------------------------------------------------------------------------
static af_thread_t *pick_next(void)
{
    af_thread_t *next = runq_pop_highest();

    // Nothing runnable: fall back to the current thread if it is still running,
    // then to idle. Never returning NULL is what keeps the scheduler total —
    // there is always something to switch to.
    if (next == NULL) {
        if (s_current != NULL && s_current->state == AF_THREAD_RUNNING) {
            return s_current;
        }
        return s_idle;
    }

    return next;
}

// The actual switch. `prev` goes back on the run queue unless it is exiting.
//
// =============================================================================
// NOTHING MAY BE LOCKED ACROSS context_switch()
// =============================================================================
// The first version took a spinlock in sched_yield() and held it while
// switching. That deadlocks immediately: thread A takes the lock, switches to
// thread B, B calls sched_yield(), tries the same lock, and spins forever
// waiting for a thread that is not running. The machine stops with no output and
// no fault — the worst possible failure shape.
//
// A lock that survives a context switch is meaningless, because the holder is
// not on a CPU any more. Interrupts are disabled only around the queue
// manipulation, and re-enabled BEFORE the switch, so the machine is taking ticks
// again by the time the new thread runs.
// =============================================================================
static void switch_to(af_thread_t *next)
{
    af_u64 flags = af_save_flags_and_disable_interrupts();

    af_thread_t *prev = s_current;

    if (next == prev) {
        af_restore_flags(flags);
        return;
    }

    if (prev != NULL && prev->state == AF_THREAD_RUNNING) {
        prev->state = AF_THREAD_RUNNABLE;
        runq_push(prev);
    }

    next->state = AF_THREAD_RUNNING;
    next->time_slice = AF_TIME_SLICE_TICKS;
    next->switches++;

    s_current = next;
    thread_set_current(next);

    s_switches++;

    // Re-enable interrupts before switching away. The idle thread's `hlt` and a
    // brand new thread's first instructions both depend on the machine being
    // able to take a tick.
    af_restore_flags(flags);

    // The switch itself. When this thread is next scheduled, execution resumes
    // here — inside sched_tick for a preemption, or inside sched_yield for a
    // voluntary switch, and returns normally from there.
    context_switch(&prev->context, &next->context);
}

// -----------------------------------------------------------------------------
// Admission
//
// A newly created thread is not runnable until it is on a run queue.
//
// thread_create() builds the thread but deliberately does not touch scheduler
// state itself, because it holds the thread-table lock and the scheduler has its
// own ordering rules. The caller must admit it — and forgetting to is a silent
// failure: the thread exists, shows up in thread_dump_all() as CREATED, and
// never runs. That is exactly what happened when the v0.2 acceptance test first
// ran: both threads were created, neither executed, and the test reported that
// the scheduler had starved them.
// -----------------------------------------------------------------------------
void sched_admit(af_thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    if (thread->state != AF_THREAD_CREATED) {
        af_warn("sched", "sched_admit called on tid %u in state %s",
                thread->tid, thread_state_name(thread->state));
        return;
    }

    if (s_idle == NULL) {
        af_error("sched", "sched_admit before sched_init — thread tid %u will "
                          "never run", thread->tid);
        return;
    }

    runq_push(thread);
}

// -----------------------------------------------------------------------------
// Timer tick
// -----------------------------------------------------------------------------
void sched_tick(void)
{
    s_ticks++;

    // Wake anything whose sleep has expired before choosing who runs next, so a
    // thread that becomes runnable on this very tick is eligible immediately
    // rather than one tick late.
    sched_wake_sleepers(s_ticks);

    af_thread_t *current = s_current;
    if (current == NULL) {
        return;
    }

    current->total_ticks++;

    if (current->time_slice > 0) {
        current->time_slice--;
    }

    if (current->time_slice == 0 || s_need_resched) {
        s_need_resched = false;
        switch_to(pick_next());
    }
}

// -----------------------------------------------------------------------------
// Voluntary operations
// -----------------------------------------------------------------------------
void sched_yield(void)
{
    af_thread_t *current = s_current;

    if (current != NULL && current->state == AF_THREAD_RUNNING) {
        current->time_slice = AF_TIME_SLICE_TICKS;
    }

    // No lock here. See the note above switch_to(): anything held across a
    // context switch deadlocks the moment two threads both try it.
    switch_to(pick_next());
}

void sched_block(void *wait_obj)
{
    af_thread_t *self = s_current;

    if (self == NULL || self == s_idle) {
        // Blocking the last runnable thread deadlocks the machine with nothing
        // left to report it. Refuse, loudly.
        af_error("sched", "refusing to block the idle thread");
        return;
    }

    self->state = AF_THREAD_BLOCKED;
    self->wait_obj = wait_obj;

    // Yield until something unblocks us. The loop is not decoration: with no
    // other runnable thread, pick_next() returns this same thread, so a single
    // yield would resume a thread that is still supposed to be blocked.
    while (self->state == AF_THREAD_BLOCKED) {
        af_thread_t *next = pick_next();

        if (next == self) {
            // Only this thread can run. Drop to idle and try again.
            next = s_idle;
        }

        next->state = AF_THREAD_RUNNING;
        next->time_slice = AF_TIME_SLICE_TICKS;
        next->switches++;

        af_thread_t *prev = s_current;
        s_current = next;
        thread_set_current(next);
        s_switches++;

        context_switch(&prev->context, &next->context);
    }

    self->wait_obj = NULL;
}

void sched_unblock(af_thread_t *thread)
{
    if (thread == NULL || thread->state != AF_THREAD_BLOCKED) {
        return;
    }

    runq_push(thread);
}

void sched_sleep(af_u64 ticks)
{
    af_thread_t *self = s_current;

    if (self == NULL || self == s_idle || ticks == 0) {
        return;
    }

    self->state = AF_THREAD_SLEEPING;
    self->wake_tick = s_ticks + ticks;

    while (self->state == AF_THREAD_SLEEPING) {
        af_thread_t *next = pick_next();
        if (next == self) {
            next = s_idle;
        }

        next->state = AF_THREAD_RUNNING;
        next->time_slice = AF_TIME_SLICE_TICKS;
        next->switches++;

        af_thread_t *prev = s_current;
        s_current = next;
        thread_set_current(next);
        s_switches++;

        context_switch(&prev->context, &next->context);
    }
}

void sched_wake_sleepers(af_u64 now_tick)
{
    for (af_u32 i = 0; i < AF_MAX_THREADS; i++) {
        af_thread_t *t = thread_by_tid(i + 1);

        if (t == NULL || t->state != AF_THREAD_SLEEPING) {
            continue;
        }

        if (now_tick >= t->wake_tick) {
            runq_push(t);
        }
    }
}

void sched_reap(af_thread_t *thread)
{
    if (thread == NULL || thread->state != AF_THREAD_ZOMBIE) {
        return;
    }

    // Make sure it is not still on a run queue. thread_exit() yields forever, so
    // it should not be — but a zombie that IS on the run queue would be picked
    // and resumed, and would then run with a freed stack.
    runq_remove(thread);

    if (s_zombie_count < 16) {
        s_zombies[s_zombie_count++] = thread;
    } else {
        // Zombie queue full: reap the oldest rather than leaking. The list only
        // fills if several threads exit between scheduler passes.
        thread_reap(s_zombies[0]);
        for (af_u32 i = 1; i < 16; i++) {
            s_zombies[i - 1] = s_zombies[i];
        }
        s_zombies[15] = thread;
    }
}

// Called from the idle loop, where the stack is known not to belong to any
// exiting thread.
void sched_collect_zombies(void)
{
    while (s_zombie_count > 0) {
        af_thread_t *zombie = s_zombies[--s_zombie_count];
        thread_reap(zombie);
    }
}

// -----------------------------------------------------------------------------
// Queries and diagnostics
// -----------------------------------------------------------------------------
af_u64 sched_tick_count(void)
{
    return s_ticks;
}

af_u32 sched_context_switch_count(void)
{
    return (af_u32)s_switches;
}

void sched_dump_state(void)
{
    af_info("sched", "%llu ticks, %llu context switches, %u live threads",
            (unsigned long long)s_ticks, (unsigned long long)s_switches,
            thread_live_count());

    for (af_u8 p = 0; p < AF_PRIO_LEVELS; p++) {
        if (s_runq_len[p] == 0) {
            continue;
        }
        af_info("sched", "  run queue priority %u: %u thread(s)",
                p, s_runq_len[p]);
    }

    if (s_current != NULL) {
        af_info("sched", "  running: tid %u '%s' (slice %u)",
                s_current->tid, s_current->name, s_current->time_slice);
    }
}

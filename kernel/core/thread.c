// SPDX-License-Identifier: MIT
// AfriyieOS — thread objects

#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/heap.h"
#include "afriyie/pmm.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/spinlock.h"
#include "afriyie/io.h"

// Implemented in kernel/arch/x86_64/context.asm
extern void context_switch(af_context_t *old, af_context_t *next);
extern void thread_trampoline(void);

// -----------------------------------------------------------------------------
// Thread table
//
// A fixed array of pointers rather than a linked list: a tid is then an index
// and lookup is O(1), and the bound is explicit and checkable.
// -----------------------------------------------------------------------------
static af_thread_t *s_threads[AF_MAX_THREADS];
static af_u32        s_thread_count = 0;
static af_spinlock_t s_table_lock = AF_SPINLOCK_INIT;

static af_thread_t  *s_current = NULL;

// Defined below; thread_adopt_current() calls it before its definition appears.
static af_tid_t register_thread(af_thread_t *thread);

af_thread_t *thread_current(void)
{
    return s_current;
}

void thread_set_current(af_thread_t *thread)
{
    s_current = thread;
}

// Adopts the calling context as a thread, without allocating a stack — it is
// already running on one. Used once, by kmain, so that the boot context is a
// schedulable object like everything else and the scheduler needs no special
// case for "the thread that was here first".
af_thread_t *thread_adopt_current(const char *name)
{
    af_thread_t *thread = (af_thread_t *)kzalloc(sizeof(af_thread_t));
    if (thread == NULL) {
        return NULL;
    }

    af_spin_lock(&s_table_lock);

    af_tid_t tid = register_thread(thread);
    if (tid == 0) {
        af_spin_unlock(&s_table_lock);
        kfree(thread);
        return NULL;
    }

    thread->state      = AF_THREAD_RUNNING;
    thread->priority   = AF_PRIO_DEFAULT;
    thread->time_slice = AF_TIME_SLICE_TICKS;
    thread->stack_base = NULL;          // not ours to free
    thread->stack_size = 0;

    af_strlcpy(thread->name, (name != NULL) ? name : "boot",
               sizeof(thread->name));

    af_spin_unlock(&s_table_lock);

    // The context fields stay zero. The first context_switch away from this
    // thread fills them in, and the first switch back restores them.
    s_current = thread;

    af_info("thread", "adopted the boot context as tid %u '%s'", tid, thread->name);

    return thread;
}

const char *thread_state_name(af_thread_state_t state)
{
    switch (state) {
    case AF_THREAD_FREE:     return "free";
    case AF_THREAD_CREATED:  return "created";
    case AF_THREAD_RUNNABLE: return "runnable";
    case AF_THREAD_RUNNING:  return "running";
    case AF_THREAD_BLOCKED:  return "blocked";
    case AF_THREAD_SLEEPING: return "sleeping";
    case AF_THREAD_ZOMBIE:   return "zombie";
    case AF_THREAD_DEAD:     return "dead";
    default:                 return "unknown";
    }
}

af_thread_t *thread_by_tid(af_tid_t tid)
{
    if (tid == 0 || tid > AF_MAX_THREADS) {
        return NULL;
    }
    return s_threads[tid - 1];
}

af_u32 thread_count(void)
{
    return s_thread_count;
}

af_u32 thread_live_count(void)
{
    af_u32 live = 0;
    for (af_u32 i = 0; i < AF_MAX_THREADS; i++) {
        if (s_threads[i] != NULL && s_threads[i]->state != AF_THREAD_DEAD) {
            live++;
        }
    }
    return live;
}

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------
static af_tid_t register_thread(af_thread_t *thread)
{
    for (af_tid_t tid = 1; tid <= AF_MAX_THREADS; tid++) {
        if (s_threads[tid - 1] == NULL) {
            s_threads[tid - 1] = thread;
            thread->tid = tid;
            s_thread_count++;
            return tid;
        }
    }
    return 0;
}

static void unregister_thread(af_thread_t *thread)
{
    if (thread == NULL || thread->tid == 0 || thread->tid > AF_MAX_THREADS) {
        return;
    }

    if (s_threads[thread->tid - 1] == thread) {
        s_threads[thread->tid - 1] = NULL;
        if (s_thread_count > 0) {
            s_thread_count--;
        }
    }
}

// -----------------------------------------------------------------------------
// Creation
// -----------------------------------------------------------------------------
af_thread_t *thread_create(const char *name, af_thread_fn entry, void *arg,
                           af_size stack_size, af_u8 priority)
{
    if (entry == NULL) {
        return NULL;
    }

    if (stack_size == 0) {
        stack_size = AF_KERNEL_STACK_SIZE;
    }

    // Round the stack up to a whole number of pages so it can be freed as
    // frames, and keep it 16-byte aligned as the ABI requires.
    stack_size = (af_size)AF_ALIGN_UP(stack_size, AF_PAGE_SIZE);

    if (priority >= AF_PRIO_LEVELS) {
        priority = AF_PRIO_DEFAULT;
    }

    af_spin_lock(&s_table_lock);

    af_thread_t *thread = (af_thread_t *)kzalloc(sizeof(af_thread_t));
    if (thread == NULL) {
        af_spin_unlock(&s_table_lock);
        af_error("thread", "cannot allocate a thread structure for '%s'",
                 (name != NULL) ? name : "?");
        return NULL;
    }

    af_u32 frames = (af_u32)(stack_size / AF_PAGE_SIZE);
    af_paddr stack_phys = pmm_alloc_frames(frames);
    if (stack_phys == AF_FRAME_INVALID) {
        kfree(thread);
        af_spin_unlock(&s_table_lock);
        af_error("thread", "cannot allocate a %u KiB stack for '%s'",
                 (af_u32)(stack_size / AF_KIB), (name != NULL) ? name : "?");
        return NULL;
    }

    af_tid_t tid = register_thread(thread);
    if (tid == 0) {
        pmm_free_frames(stack_phys, frames);
        kfree(thread);
        af_spin_unlock(&s_table_lock);
        af_error("thread", "thread table is full (%u threads)", (af_u32)AF_MAX_THREADS);
        return NULL;
    }

    thread->stack_base = (af_u8 *)(af_uptr)stack_phys;
    thread->stack_size = (af_u32)stack_size;
    thread->state      = AF_THREAD_CREATED;
    thread->priority   = priority;
    thread->time_slice = AF_TIME_SLICE_TICKS;
    thread->next       = NULL;
    thread->wait_obj   = NULL;

    af_strlcpy(thread->name, (name != NULL) ? name : "thread",
               sizeof(thread->name));

    // --- build the initial context -------------------------------------------
    //
    // context_switch() ends with `ret`, popping the incoming thread's stack. For
    // a thread that has never run, that address must be the trampoline, and the
    // entry function and its argument must already be in r12 and r13 — the two
    // callee-saved registers the trampoline reads them from.
    //
    // STACK ALIGNMENT. context_switch's `ret` pops eight bytes, exactly like a
    // `call`. So rsp before the ret must be 16-byte aligned, which puts rsp at
    // 8 mod 16 on entry to the trampoline — what the SysV ABI requires at a
    // function's first instruction. Both registers and the SSE code paths that
    // compiled C assumes are correct get this for free, provided the stack top is
    // 16-byte aligned and the trampoline address is pushed once.
    af_uptr stack_top = (af_uptr)(thread->stack_base + thread->stack_size);
    stack_top &= ~(af_uptr)0xF;                 // 16-byte aligned

    stack_top -= 16;                            // room for the return address
    *((af_u64 *)stack_top) = (af_u64)(af_uptr)&thread_trampoline;

    af_memset(&thread->context, 0, sizeof(thread->context));
    thread->context.rsp = stack_top;
    thread->context.r12 = (af_u64)(af_uptr)entry;   // the trampoline calls this
    thread->context.r13 = (af_u64)(af_uptr)arg;     // ...with this argument
    thread->context.rbp = 0;                        // terminate a stack walk here

    af_spin_unlock(&s_table_lock);

    af_info("thread", "created tid %u '%s': %u KiB stack at 0x%lX, priority %u",
            tid, thread->name, (af_u32)(stack_size / AF_KIB), stack_phys, priority);

    return thread;
}

// -----------------------------------------------------------------------------
// Termination
// -----------------------------------------------------------------------------
//
// A thread cannot free the stack it is standing on, so exiting marks the thread
// a ZOMBIE and yields away forever. The scheduler reaps it on the next pass,
// from a different stack.
AF_NORETURN void thread_exit(void)
{
    af_thread_t *self = s_current;

    if (self == NULL) {
        af_panic("thread_exit called with no current thread");
    }

    af_info("thread", "tid %u '%s' exiting after %llu ticks and %llu switches",
            self->tid, self->name,
            (unsigned long long)self->total_ticks,
            (unsigned long long)self->switches);

    self->state = AF_THREAD_ZOMBIE;

    // Never returns: the scheduler drops a zombie from the run queue, so there
    // is nothing left to switch back to.
    for (;;) {
        sched_yield();
    }
}

// Called from the assembly trampoline when a thread's entry function returns.
// A thread that falls off the end of its function must still terminate.
void thread_exit_from_asm(void)
{
    thread_exit();
}

void thread_reap(af_thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    af_spin_lock(&s_table_lock);

    thread->state = AF_THREAD_DEAD;

    af_u32 frames = thread->stack_size / AF_PAGE_SIZE;
    if (thread->stack_base != NULL) {
        pmm_free_frames((af_paddr)(af_uptr)thread->stack_base, frames);
        thread->stack_base = NULL;
    }

    unregister_thread(thread);

    af_spin_unlock(&s_table_lock);

    // The structure itself goes last: unregister_thread() touches it, and so
    // does the caller's loop.
    kfree(thread);
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
void thread_dump_all(void)
{
    af_info("thread", "%u threads (%u registered):",
            thread_live_count(), s_thread_count);

    for (af_u32 i = 0; i < AF_MAX_THREADS; i++) {
        af_thread_t *t = s_threads[i];
        if (t == NULL) {
            continue;
        }

        af_info("thread", "  tid %-3u %-16s %-9s prio %u  ticks %-8llu "
                          "switches %llu",
                t->tid, t->name, thread_state_name(t->state), t->priority,
                (unsigned long long)t->total_ticks,
                (unsigned long long)t->switches);
    }
}

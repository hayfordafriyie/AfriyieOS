// SPDX-License-Identifier: MIT
// AfriyieOS — threads

#ifndef AFRIYIE_THREAD_H
#define AFRIYIE_THREAD_H

#include "types.h"
#include "status.h"
#include "config.h"

typedef af_u32 af_tid_t;

typedef enum {
    AF_THREAD_FREE = 0,
    AF_THREAD_CREATED,
    AF_THREAD_RUNNABLE,
    AF_THREAD_RUNNING,
    AF_THREAD_BLOCKED,
    AF_THREAD_SLEEPING,
    AF_THREAD_ZOMBIE,     // exited, waiting to be reaped
    AF_THREAD_DEAD,
} af_thread_state_t;

// The switch context.
//
// =============================================================================
// THE FIELD ORDER IS THE ASSEMBLY CONTRACT
// =============================================================================
// kernel/arch/x86_64/context.asm loads and stores these at fixed offsets. The
// static asserts below pin the layout, because a mismatch here compiles
// perfectly and produces a thread that resumes with one wrong register — a
// failure that appears thousands of switches later, in unrelated code.
//
// Only callee-saved registers are stored. The SysV ABI already requires a
// caller to treat everything else as clobbered across a function call, and a
// context switch is entered as a function call.
// =============================================================================
typedef struct {
    af_u64 rbx;      // offset 0
    af_u64 rbp;      // offset 8
    af_u64 r12;      // offset 16
    af_u64 r13;      // offset 24
    af_u64 r14;      // offset 32
    af_u64 r15;      // offset 40
    af_u64 rsp;      // offset 48
    af_u64 rip;      // offset 56 — only used when building an initial context
} af_context_t;

AF_STATIC_ASSERT_OFFSET(af_context_t, rbx, 0);
AF_STATIC_ASSERT_OFFSET(af_context_t, rbp, 8);
AF_STATIC_ASSERT_OFFSET(af_context_t, r12, 16);
AF_STATIC_ASSERT_OFFSET(af_context_t, r13, 24);
AF_STATIC_ASSERT_OFFSET(af_context_t, r14, 32);
AF_STATIC_ASSERT_OFFSET(af_context_t, r15, 40);
AF_STATIC_ASSERT_OFFSET(af_context_t, rsp, 48);

struct af_thread;

typedef struct af_thread {
    af_context_t        context;
    af_tid_t            tid;
    af_thread_state_t   state;
    af_u8               priority;
    af_u8               _pad[3];
    af_u32              time_slice;      // ticks remaining before preemption

    af_u8              *stack_base;      // lowest address of the stack region
    af_u32              stack_size;

    af_u64              wake_tick;       // for AF_THREAD_SLEEPING
    af_u64              total_ticks;     // CPU time consumed, for accounting
    af_u64              switches;        // times this thread has been scheduled

    void               *wait_obj;        // what it is blocked on, for diagnostics
    char                name[AF_THREAD_NAME_LEN];

    // The page-table root this thread runs on, or 0 for the kernel's own.
    //
    // A user thread carries a private address space; every kernel thread shares
    // the kernel's. The scheduler installs this on each switch, so a thread's
    // memory is a property of the thread rather than of whatever ran last —
    // which is what makes "the kernel is mapped everywhere" a fact the kernel
    // can rely on instead of a hope.
    af_u64              addr_space;

    struct af_thread   *next;            // run-queue link
} af_thread_t;

// Runs on the current stack and never returns.
typedef void (*af_thread_fn)(void *arg);

// Allocates the thread structure and its kernel stack from the heap.
// Returns NULL when memory is exhausted or the table is full.
af_thread_t *thread_create(const char *name, af_thread_fn entry, void *arg,
                           af_size stack_size, af_u8 priority);

// Adopts the calling context as a thread without allocating a stack, since it is
// already running on one. Used once by kmain so the boot context is an ordinary
// schedulable thread.
af_thread_t *thread_adopt_current(const char *name);

void thread_set_current(af_thread_t *thread);

// Terminates the calling thread. The scheduler reaps the structure once the
// thread has stopped running, because a thread cannot free the stack it is
// standing on.
AF_NORETURN void thread_exit(void);

// Terminates the calling thread when its entry function returns. Called from the
// assembly trampoline in kernel/arch/x86_64/context.asm, so it lives here rather
// than being static to thread.c.
void thread_exit_from_asm(void);

// Called by the scheduler when a zombie's stack is no longer in use.
void thread_reap(af_thread_t *thread);

af_thread_t *thread_current(void);
af_thread_t *thread_by_tid(af_tid_t tid);
af_u32       thread_count(void);
af_u32       thread_live_count(void);

const char *thread_state_name(af_thread_state_t state);

// Diagnostics
void thread_dump_all(void);

#endif // AFRIYIE_THREAD_H

// SPDX-License-Identifier: MIT
// AfriyieOS — scheduler

#ifndef AFRIYIE_SCHED_H
#define AFRIYIE_SCHED_H

#include "types.h"
#include "status.h"
#include "thread.h"

// Brings up the run queues.
//
// `boot_thread` is the context the caller is running on and becomes an ordinary
// schedulable thread. `idle_thread` is a SEPARATE thread that halts the CPU when
// nothing else can run — it must not be the same object, because sched_sleep()
// and sched_block() refuse to act on the idle thread, so a boot context that is
// also idle turns every sleep in the boot path into a silent no-op.
//
// Neither thread is admitted automatically; the caller calls sched_admit() for
// the idle thread.
af_status_t sched_init(af_thread_t *boot_thread, af_thread_t *idle_thread);

// Called on every timer tick. Decrements the running thread's slice and demands
// a reschedule when it expires. Runs in interrupt context: it must not allocate
// and must not block.
void sched_tick(void);

// Puts a newly created thread on a run queue, making it eligible to run.
// MUST be called by whoever created the thread: thread_create() does not do it
// itself, because it holds the thread-table lock and the scheduler has its own
// lock ordering. A thread that is created and never admitted exists, appears in
// the thread dump as CREATED, and never runs.
void sched_admit(af_thread_t *thread);

// Picks the next runnable thread and switches to it. Safe to call from a normal
// kernel thread; returns when this thread is scheduled again.
void sched_yield(void);

// Blocks the calling thread on `wait_obj` until thread_unblock() is called.
// A no-op when the thread is the only one that can run — blocking the last
// runnable thread would deadlock the machine with no diagnostic.
void sched_block(void *wait_obj);

// Moves a blocked thread back to the run queue and wakes it.
void sched_unblock(af_thread_t *thread);

// Sleeps for at least `ticks` timer ticks.
void sched_sleep(af_u64 ticks);

// Called by the timer tick to wake threads whose sleep has expired.
void sched_wake_sleepers(af_u64 now_tick);

// Removes a terminated thread from the run queue and reaps it.
void sched_reap(af_thread_t *thread);

// Frees zombie thread structures. MUST be called from a context that does not
// belong to an exiting thread — the idle loop — because reaping frees the stack
// the exiting thread was standing on.
void sched_collect_zombies(void);

af_u64 sched_tick_count(void);
af_u32 sched_context_switch_count(void);

void sched_dump_state(void);

#endif // AFRIYIE_SCHED_H

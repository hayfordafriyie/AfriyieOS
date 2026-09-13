// SPDX-License-Identifier: MIT
// AfriyieOS — processes
//
// A PROCESS OWNS AN ADDRESS SPACE AND A SET OF THREADS. A thread does not.
//
// That distinction is the whole reason this file exists, and it was wrong until
// now. At v0.5 the address space belonged to the *thread*, which was enough to
// run one program and exactly the wrong shape for anything else:
//
//   * Two threads of one program would not have shared memory, so a program
//     could not have a second thread at all.
//   * Nothing could outlive its thread, so "the process exited" had no meaning.
//   * There was nowhere to hang a capability table, and there has to be: a
//     capability is authority held by a PROGRAM, not by whichever of its threads
//     happens to be running. See docs/architecture/capability-model.md and
//     ADR-012 — every foreign personality is a process holding capabilities.
//
// So the address space moved up, and the thread kept a pointer to its owner. A
// thread with no process is a kernel thread and runs on the kernel's tables;
// that is what every boot-time thread is, and it is not a special case so much
// as the absence of one.
//
// WHAT IS HERE: creation, thread membership, exit, blocking wait, reaping, and
// teardown of the address space when the last thread is gone.
//
// WHAT IS NOT HERE, deliberately:
//   * fork() and copy-on-write — needs the VMM to be able to clone a space
//     lazily, which is its own piece of work and its own set of tests.
//   * exec() replacing an image in a live process — the current path creates a
//     process with one thread and loads into it, which is the same thing for a
//     program that has not started yet.
//   * Capability tables — the field is not declared until it can be used,
//     because a placeholder that nothing enforces is worse than an absence.

#ifndef AFRIYIE_PROCESS_H
#define AFRIYIE_PROCESS_H

#include "types.h"
#include "status.h"
#include "hal.h"

struct af_thread;

#define AF_PROCESS_NAME_LEN 24

typedef af_u32 af_pid_t;

#define AF_PID_INVALID 0u
#define AF_PID_KERNEL  1u   // reserved: the boot context, which has no process

typedef enum {
    AF_PROCESS_FREE = 0,
    AF_PROCESS_ALIVE,
    AF_PROCESS_ZOMBIE,    // exited; waiting for a parent, or for the reaper
    AF_PROCESS_DEAD,
} af_process_state_t;

typedef struct af_process {
    af_pid_t            pid;
    af_process_state_t  state;
    af_pid_t            parent;

    // The page-table root this process runs on. Owned here and nowhere else.
    // 0 means the kernel's own tables, which only a kernel process would have —
    // and no kernel process exists yet, so in practice this is always non-zero
    // for a live process.
    hal_pt_root_t       addr_space;

    // The process's threads, singly linked through af_thread.process_next.
    // thread_count is maintained rather than derived, because it is read on
    // every thread exit and walking a list to count is how a small correctness
    // detail becomes an O(n) operation in the hottest path there is.
    struct af_thread   *threads;
    af_u32              thread_count;

    af_i32              exit_code;

    // A thread blocked in process_wait on this process, or NULL.
    //
    // One waiter, not a list. A process can only be waited on by its parent, and
    // a parent can only wait on one child at a time — anything more would be
    // inventing a general wait queue for a case that cannot arise yet, and the
    // abstraction should arrive with the case.
    struct af_thread   *waiter;

    af_u64              created_tick;
    char                name[AF_PROCESS_NAME_LEN];
} af_process_t;

// Clears the table. Called once, from kmain, before any thread runs that might
// create a process.
void process_init(void);

// Creates a process with its own address space and no threads.
//
// `parent` of AF_PID_KERNEL means a process not owned by any user program —
// what init is. Returns NULL when the table is full or the address space could
// not be created; the caller must handle NULL, because the alternative is a
// kernel that panics when a user asks for one program too many.
af_process_t *process_create(const char *name, af_pid_t parent);

// Adds and removes a thread. The scheduler calls these; nothing else should.
void process_attach_thread(af_process_t *proc, struct af_thread *thread);
void process_detach_thread(struct af_thread *thread);

// The process the calling thread belongs to, or NULL for a kernel thread.
af_process_t *process_current(void);

af_process_t *process_find(af_pid_t pid);

// Ends the calling process: sets the exit code, wakes a waiter if there is one,
// and terminates the calling thread. Never returns.
//
// The address space is NOT freed here. The thread that calls this is standing on
// a stack in that space's kernel mapping; freeing the tables under it is an
// instant triple fault. Teardown happens when the last thread has been reaped,
// from a context that is provably not the exiting one.
AF_NORETURN void process_exit(af_i32 code);

// Waits for a child to exit and reaps it. Blocks until it does.
//
// Returns AF_ERR_NOENT if `pid` is not a child of the caller, and AF_ERR_INVAL
// if it is not a process at all. A child that has already exited returns
// immediately, which is the case that must not be missed: a wait that only works
// when it arrives first is a race, and a short-lived child always loses it.
af_status_t process_wait(af_pid_t pid, af_i32 *out_code);

// Frees a zombie process and everything it owns. Called by the reaper, not by a
// parent — a parent's wait only marks it reaped.
void process_reap(af_process_t *proc);

void process_dump(void);

// How many processes are alive. For the self test and for diagnostics.
af_u32 process_live_count(void);

// Boot-time checks: creation, isolation between two address spaces, thread
// membership, exit, wait and teardown. Emits AF_PROC_OK.
void af_process_selftest(void);

#endif // AFRIYIE_PROCESS_H

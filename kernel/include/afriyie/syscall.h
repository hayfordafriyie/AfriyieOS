// SPDX-License-Identifier: MIT
// AfriyieOS — user mode and system calls

#ifndef AFRIYIE_SYSCALL_H
#define AFRIYIE_SYSCALL_H

#include "types.h"
#include "status.h"

// Handles a ring-3 system call. Called from the interrupt dispatcher for the
// 0x80 vector, with the frame isr_common built.
void syscall_entry(void *frame);

af_u64 usermode_syscall_count(void);
void   usermode_dump_stats(void);

// The v0.4 acceptance test: map a user page and a user stack, build a small
// ring-3 program in it, drop to ring 3, and let it make system calls.
void usermode_selftest(void);

#endif // AFRIYIE_SYSCALL_H

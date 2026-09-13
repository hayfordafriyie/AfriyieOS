// SPDX-License-Identifier: MIT
// AfriyieOS — build and kernel configuration

#ifndef AFRIYIE_CONFIG_H
#define AFRIYIE_CONFIG_H

// AF_KIB is defined in types.h, and the page-size sanity check at the bottom of
// this file uses it. Without this include the preprocessor silently treats the
// unknown identifier as 0, and `#if AF_PAGE_SIZE != 4096` fires on a correct
// build. That is a genuinely nasty failure: the error message is right there but
// the cause is a missing include twelve lines up.
#include "types.h"

// -----------------------------------------------------------------------------
// Identity
// -----------------------------------------------------------------------------
#define AF_NAME             "AfriyieOS"
#define AF_VERSION_MAJOR    0
#define AF_VERSION_MINOR    1
#define AF_VERSION_PATCH    0
#define AF_VERSION_STRING   "0.1.0"
#define AF_CODENAME         "Seed"

// -----------------------------------------------------------------------------
// Architecture selection
//
// Exactly one of these must be 1. The values are set by CMake
// (see cmake/flags.cmake) so the kernel is built once per target.
// -----------------------------------------------------------------------------
#ifndef AF_TARGET_X86_64
#  define AF_TARGET_X86_64 1
#endif
#ifndef AF_TARGET_AARCH64
#  define AF_TARGET_AARCH64 0
#endif

#if (AF_TARGET_X86_64 + AF_TARGET_AARCH64) != 1
#  error "Exactly one AF_TARGET_* must be enabled"
#endif

// -----------------------------------------------------------------------------
// Memory model — identical on both architectures (see blueprint section 3.3)
// -----------------------------------------------------------------------------
#define AF_PAGE_SIZE        (4 * AF_KIB)
#define AF_PAGE_SHIFT       12
#define AF_PAGE_MASK        (AF_PAGE_SIZE - 1)

#define AF_HUGE_PAGE_SIZE   (2 * AF_MIB)
#define AF_HUGE_PAGE_SHIFT  21

// Kernel lives in the higher half, physical RAM is direct-mapped below it.
#define AF_KERNEL_BASE      0xFFFFFFFF80000000ULL
#define AF_DIRECT_MAP_BASE  0xFFFF800000000000ULL
#define AF_KERNEL_HEAP_BASE 0xFFFFC00000000000ULL

// User address space.
//
// THE TOP-LEVEL SLOT IS PART OF THE CONTRACT, not an implementation detail.
//
// The kernel's identity map (0..3 GiB) lives in PML4[0]. If user pages also
// lived there they would share a top-level entry with it — and the x86 walk
// requires the USER bit at EVERY level on the path to a user page, so mapping one
// user page anywhere in PML4[0] sets USER on PML4[0] itself. One entry, then, with
// two owners and no way to tell them apart.
//
// That ambiguity is what a process address space has to resolve, and resolving it
// by copying the tree and deciding ownership entry-by-entry does not work: the
// USER bit on an intermediate entry means "something below me is the user's", not
// "all of me is". Getting it wrong in one direction drops the kernel out of the
// process (a triple fault on the next instruction fetch); getting it wrong in the
// other frees the kernel's own frames when the process dies.
//
// So the layout separates them instead. PML4[1] is the user region and belongs to
// whoever owns the address space. PML4[0] and PML4[2..511] are the kernel's, and a
// process shares them by pointer — which needs no copying, no ownership rule, and
// no agreement between the map path and the free path beyond "index 1 is not
// yours". hal_pt_destroy already had exactly that rule.
#define AF_USER_PML4_INDEX  1
#define AF_USER_REGION_BASE 0x0000008000000000ULL   // 512 GiB — PML4[1]
#define AF_USER_BASE        0x0000000000001000ULL   // page zero is never mapped
#define AF_USER_TOP         0x000000FFFFFFFFFFFULL  // top of PML4[1] — 1 TiB - 1
#define AF_USER_STACK_SIZE  (8 * AF_MIB)

// Addresses at or above this are the kernel's, whatever the USER bit says.
#define AF_KERNEL_REGION_BASE AF_USER_REGION_BASE

// -----------------------------------------------------------------------------
// Kernel limits
// -----------------------------------------------------------------------------
#define AF_MAX_CPUS         8
#define AF_MAX_PROCESSES    256
#define AF_MAX_THREADS      1024
#define AF_MAX_CAPS         256       // capability slots per process
#define AF_THREAD_NAME_LEN  32

#define AF_KERNEL_STACK_SIZE   (16 * AF_KIB)
#define AF_IST_STACK_SIZE      (8 * AF_KIB)   // x86_64 double-fault / NMI stacks

#define AF_PRIO_LEVELS      8         // 0 = highest priority, 7 = lowest
#define AF_PRIO_DEFAULT     4
#define AF_PRIO_IDLE        7

#define AF_SCHED_TICK_HZ    100       // 10 ms time slice
#define AF_TIME_SLICE_TICKS 2         // 20 ms per thread at default priority

// -----------------------------------------------------------------------------
// Subsystem enable flags for v0.1
// -----------------------------------------------------------------------------
#define AF_WITH_SERIAL      1
#define AF_WITH_FRAMEBUFFER 1
#define AF_WITH_PMM         0   // v0.1: not yet — see blueprint section 11
#define AF_WITH_VMM         0
#define AF_WITH_HEAP        0
#define AF_WITH_SCHED       0
#define AF_WITH_IDT         0
#define AF_WITH_SYSCALL     0
#define AF_WITH_IPC         0

// -----------------------------------------------------------------------------
// Boot markers — asserted by CI over the serial console
// -----------------------------------------------------------------------------
#define AF_BOOT_MARKER_OK       "AF_BOOT_OK"
#define AF_BOOT_MARKER_PANIC    "AF_PANIC:"
#define AF_BOOT_MARKER_FAIL     "AF_TEST_FAIL:"

// -----------------------------------------------------------------------------
// Sanity checks
// -----------------------------------------------------------------------------
#if AF_PAGE_SIZE != 4096
#  error "AfriyieOS currently assumes 4 KiB pages"
#endif

#if AF_SCHED_TICK_HZ < 10 || AF_SCHED_TICK_HZ > 1000
#  error "AF_SCHED_TICK_HZ must be between 10 and 1000"
#endif

#endif // AFRIYIE_CONFIG_H

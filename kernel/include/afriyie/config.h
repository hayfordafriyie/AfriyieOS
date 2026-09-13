// SPDX-License-Identifier: MIT
// AfriyieOS — build and kernel configuration

#ifndef AFRIYIE_CONFIG_H
#define AFRIYIE_CONFIG_H

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

// User address space occupies the low half.
#define AF_USER_BASE        0x0000000000001000ULL   // page zero is never mapped
#define AF_USER_TOP         0x00007FFFFFFFFFFFULL
#define AF_USER_STACK_SIZE  (8 * AF_MIB)

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

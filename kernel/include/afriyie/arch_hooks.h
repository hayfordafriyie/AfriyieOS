// SPDX-License-Identifier: MIT
// AfriyieOS — architecture hook declarations
//
// These functions are implemented by the per-architecture layer
// (kernel/arch/<arch>/). The kernel core calls them through this header only.
// Weak default implementations live in kernel/core/arch_hooks.c so that the
// core still links when a hook has not been written yet for a new target.

#ifndef AFRIYIE_ARCH_HOOKS_H
#define AFRIYIE_ARCH_HOOKS_H

#include "types.h"
#include "boot_info.h"

// The kernel entry point, called by the boot bridge with interrupts disabled on
// a private stack and no runtime environment of any kind. Never returns.
//
// Declared here because both sides of the boundary need the type: kmain.c
// defines it, and the entry stub in kernel/arch/<arch>/ calls it. Without a
// declaration the definition trips -Wmissing-declarations, which is exactly the
// warning that catches a signature that has drifted away from its caller.
void kmain(af_boot_info_t *boot_info);

// Dumps architecture-specific CPU state (registers, fault address, stack trace)
// during a panic. Called by af_panic_at() after the message is printed.
void af_arch_panic_dump(void);

// Logs vendor/model/feature information about the running CPU.
void af_arch_cpu_dump(void);

// Brings up the earliest possible console, before hal_console_init() runs.
// On x86_64 this is the COM1 UART; on ARM64 the PL011 from the device tree.
void af_arch_early_console_init(const af_boot_info_t *bi);

// Writes a single character to the arch console with no buffering.
// Used by the panic path, which must not depend on the log layer.
void af_arch_console_putc(char c);

// Terminates the machine after a panic. Must be a hard stop: interrupts
// disabled, no return. Never falls back to a reset, which would hide the fault.
AF_NORETURN void af_arch_panic_halt(void);

#endif // AFRIYIE_ARCH_HOOKS_H

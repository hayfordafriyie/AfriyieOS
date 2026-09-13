// SPDX-License-Identifier: MIT
// AfriyieOS — Hardware Abstraction Layer
//
// =============================================================================
// THE PORTABILITY CONTRACT
// =============================================================================
//
// Everything above this header is architecture-independent. Everything below is
// per-architecture. Every function declared here has exactly one implementation
// per target (kernel/arch/x86_64/ or kernel/arch/arm64/) and the prototype may
// not change without an ADR.
//
// The rule that keeps the project honest (blueprint section 11, v1.1):
//   If porting to a second architecture requires changing more than 20% of the
//   architecture-independent code, this interface is wrong and must be redesigned.

#ifndef AFRIYIE_HAL_H
#define AFRIYIE_HAL_H

#include "types.h"
#include "boot_info.h"
#include "status.h"   // af_status_t, the return type of most of this interface

// =============================================================================
// Early boot — called by kmain() in this exact order
// =============================================================================

// Console first: without it, a failure in any later step is invisible.
af_status_t hal_console_init(const af_boot_info_t *bi);

// CPU tables, privilege setup, exception vectors.
af_status_t hal_cpu_init(void);

// Interrupt controller bring-up (PIC/APIC, or GIC).
af_status_t hal_interrupt_init(void);

// Cores. v0.1 runs single-core; SMP arrives in v1.2.
af_status_t hal_smp_init(void);
af_u32      hal_cpu_count(void);
af_u32      hal_cpu_id(void);

// =============================================================================
// Time
// =============================================================================

// Programs the periodic scheduler tick at `hz` and enables the timer IRQ.
af_status_t hal_timer_init(af_u32 hz);

// Monotonic nanoseconds since boot. Must be callable from interrupt context.
af_u64 hal_time_ns(void);

// Busy-wait for approximately `us` microseconds. Used only during early boot,
// before the scheduler exists.
void hal_delay_us(af_u64 us);
void hal_delay_ms(af_u64 ms);

// =============================================================================
// Interrupts
// =============================================================================

// Interrupt handler signature. Returning `true` means "I handled this IRQ";
// `false` lets the next chained handler try, and eventually the spurious-IRQ
// path runs.
typedef bool (*hal_irq_handler_fn)(af_u32 irq, void *ctx);

// Registers a handler on a hardware IRQ line. Returns AF_ERR_TOOMANY when the
// per-line chain is full.
af_status_t hal_irq_register(af_u32 irq, hal_irq_handler_fn fn, void *ctx);
af_status_t hal_irq_unregister(af_u32 irq, hal_irq_handler_fn fn);

// Masks/unmasks a line and acknowledges an in-service interrupt. A driver that
// does not ack will never see a second interrupt.
void hal_irq_enable(af_u32 irq);
void hal_irq_disable(af_u32 irq);
void hal_irq_ack(af_u32 irq);
void hal_irq_set_priority(af_u32 irq, af_u8 priority);

// Global interrupt state, for the spinlock implementation.
af_u64 hal_irq_save(void);
void   hal_irq_restore(af_u64 flags);
void   hal_irq_enable_globally(void);
void   hal_irq_disable_globally(void);

#define HAL_IRQ_COUNT 256

// Generic IRQ dispatch. Defined in kernel/core/irq.c; called by the per-arch
// interrupt entry once it has worked out which IRQ line fired.
void irq_dispatch(af_u32 irq);
void irq_dump_stats(void);

// =============================================================================
// Memory — architecture-specific page table manipulation
//
// The generic VMM (kernel/core/vmm.c, v0.2) drives these. The HAL knows about
// PML4/TTBR; the VMM knows about address spaces and mappings.
// =============================================================================

typedef af_u64 hal_pt_root_t;   // CR3 value / TTBR0 value

// Page mapping flags — one portable set, translated inside the HAL.
#define HAL_PRESENT   (1u << 0)
#define HAL_WRITABLE  (1u << 1)
#define HAL_USER      (1u << 2)
#define HAL_EXEC      (1u << 3)
#define HAL_GLOBAL    (1u << 4)
#define HAL_NOCACHE   (1u << 5)
#define HAL_DEVICE    (1u << 6)
#define HAL_HUGE      (1u << 7)

// Allocates a zeroed page-table root and returns its physical address.
hal_pt_root_t hal_pt_create(void);

// Frees a page-table root and every table it owns.
void hal_pt_destroy(hal_pt_root_t root);

af_status_t hal_map_page(hal_pt_root_t root, af_vaddr va, af_paddr pa, af_u32 flags);
af_status_t hal_map_range(hal_pt_root_t root, af_vaddr va, af_paddr pa,
                          af_u64 size, af_u32 flags);
af_status_t hal_unmap_page(hal_pt_root_t root, af_vaddr va);
void        hal_unmap_range(hal_pt_root_t root, af_vaddr va, af_u64 size);

// Returns 0 when the address is unmapped.
af_paddr hal_translate(hal_pt_root_t root, af_vaddr va);

// Flags of the mapping covering `va`, or 0 when unmapped.
af_u32 hal_query_flags(hal_pt_root_t root, af_vaddr va);

// Installs a page-table root (CR3 / TTBR0_EL1 + barrier).
void hal_set_page_table(hal_pt_root_t root);
hal_pt_root_t hal_get_page_table(void);

// Builds the kernel's own page tables and installs them.
//
// Until this runs the kernel is executing on whatever page tables the firmware
// left behind. That works by accident — the firmware's identity map happens to
// cover us — but the kernel does not own its address space and cannot create a
// second one, which is what user mode needs.
void hal_paging_init(const af_boot_info_t *bi);

// The identity/direct map installed at boot: kernel virtual → physical.
af_paddr hal_virt_to_phys(af_vaddr va);
af_vaddr hal_phys_to_virt(af_paddr pa);

// =============================================================================
// CPU
// =============================================================================

// Writes the stack pointer the CPU must load when an interrupt arrives while
// the thread was in user mode (x86_64 TSS.RSP0). No-op on ARM64, where the
// exception level's SP is selected by hardware.
void hal_set_kernel_stack(af_vaddr top);
af_vaddr hal_get_kernel_stack(void);

// Saves/restores the FPU/SIMD state around a context switch. v0.1 does not use
// floating point in the kernel, but a user thread may.
void hal_fpu_init(void);
void hal_fpu_save(void *area);
void hal_fpu_restore(const void *area);
af_size hal_fpu_state_size(void);

// Idle the current CPU until the next interrupt.
AF_NORETURN void hal_idle_forever(void);

// Full system shutdown / reset (ACPI on PC, PSCI on ARM64 once implemented).
AF_NORETURN void hal_system_shutdown(void);
AF_NORETURN void hal_system_reset(void);

// -----------------------------------------------------------------------------
// Which implementation is linked in
// -----------------------------------------------------------------------------
#if AF_TARGET_X86_64
#  define HAL_ARCH_NAME "x86_64"
#elif AF_TARGET_AARCH64
#  define HAL_ARCH_NAME "aarch64"
#endif

// Prints the CPU identity (vendor, model, feature flags) to the console.
void hal_cpu_dump_info(void);

#endif // AFRIYIE_HAL_H

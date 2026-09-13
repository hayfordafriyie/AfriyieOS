// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 internal architecture header
//
// Not part of the public kernel interface: nothing outside kernel/arch/x86_64/
// and the arch dispatch in kernel/CMakeLists.txt should include this.

#ifndef AFRIYIE_ARCH_X86_64_H
#define AFRIYIE_ARCH_X86_64_H

#include "afriyie/types.h"
#include "afriyie/status.h"   // af_status_t, the return type of the init entry points

// =============================================================================
// Serial console (COM1)
// =============================================================================
void af_x86_serial_init(af_u16 port, af_u32 baud);
void af_x86_serial_putc(char c);
void af_x86_serial_write(const char *text, af_size len);
bool af_x86_serial_is_ready(void);

// Log sink adapter: wires the serial writer into af_log_set_sink().
void af_x86_log_sink(const char *text, af_size len, void *ctx);

// =============================================================================
// 8254 PIT timer
// =============================================================================
void  af_x86_pit_init(af_u32 hz);
void  af_x86_pit_tick(void);      // called from the IRQ0 handler
af_u64 af_x86_pit_ticks(void);
af_u32 af_x86_pit_hz(void);
af_u64 af_x86_time_ns(void);

#define AF_COM1_PORT 0x3F8

// =============================================================================
// GDT
// =============================================================================
typedef struct AF_PACKED {
    af_u16 limit_low;
    af_u16 base_low;
    af_u8  base_middle;
    af_u8  access;
    af_u8  granularity;
    af_u8  base_high;
} af_gdt_entry_t;

typedef struct AF_PACKED {
    af_u16 limit;
    af_u64 base;
} af_gdt_pointer_t;

// Selector layout: 0x00 null, 0x08 kernel code, 0x10 kernel data,
//                  0x18 user code,   0x20 user data,  0x28 TSS
#define AF_GDT_KERNEL_CODE 0x08
#define AF_GDT_KERNEL_DATA 0x10
#define AF_GDT_USER_CODE   0x18
#define AF_GDT_USER_DATA   0x20
#define AF_GDT_TSS         0x28

// Implemented in cpu.asm
void gdt_load(const af_gdt_pointer_t *gdtr);

// Builds and installs the GDT. Called from hal_cpu_init().
af_status_t af_x86_gdt_init(void);

// =============================================================================
// IDT
// =============================================================================

// The normalised interrupt frame. The field order is dictated by the push
// order in isr.asm and must never be changed without changing that file.
typedef struct AF_PACKED {
    // Pushed by isr_common, in the order they end up in memory.
    af_u64 r15, r14, r13, r12, r11, r10, r9, r8;
    af_u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Pushed by the stub / the CPU.
    af_u64 vector;
    af_u64 error_code;

    // Pushed by the CPU on every interrupt.
    af_u64 rip;
    af_u64 cs;
    af_u64 rflags;

    // Pushed by the CPU only when the interrupt changes privilege level
    // (i.e. comes from user mode). Valid only when `from_user` is true.
    af_u64 rsp;
    af_u64 ss;
} isr_frame_t;

// True when the interrupted context was in user mode (CPL 3).
bool isr_frame_from_user(const isr_frame_t *frame);

// Builds and installs the IDT with all 256 vectors pointing at their stubs.
af_status_t af_x86_idt_init(void);

// Called from isr_common. Defined in isr.c.
void isr_dispatch(isr_frame_t *frame);

// Records the frame captured by isr_dispatch so the panic dump can print the
// register set that faulted. Separate from isr_dispatch because the panic path
// is in the generic core, which knows nothing about x86_64 frames.
void af_x86_set_fault_frame(const isr_frame_t *frame);

// Human-readable exception name, e.g. "#PF page fault". Never returns NULL.
const char *af_x86_exception_name(af_u64 vector);

// --- 8259 PIC control, used by the generic IRQ layer in kernel/core/irq.c -----
#define AF_X86_PIC_IRQ_BASE 0x20
#define AF_X86_PIC_IRQ_COUNT 16

void af_x86_pic_mask_all(void);
void af_x86_pic_enable(af_u32 irq);
void af_x86_pic_disable(af_u32 irq);
void af_x86_pic_ack(af_u32 irq);
bool af_x86_pic_is_masked(af_u32 irq);

// Decodes the x86_64 page-fault error code into a written description.
void af_x86_describe_page_fault(af_u64 error_code, char *out, af_size out_size);

// =============================================================================
// CPU identification
// =============================================================================
typedef struct {
    char   vendor[13];      // 12 characters + NUL
    char   brand[49];       // 48 characters + NUL
    af_u32 family;
    af_u32 model;
    af_u32 stepping;
    af_u32 logical_cpus;
    af_u64 feature_edx;
    af_u64 feature_ecx;
    af_u64 feature_ext;
} af_x86_cpu_info_t;

void af_x86_cpu_detect(af_x86_cpu_info_t *out);

#endif // AFRIYIE_ARCH_X86_64_H

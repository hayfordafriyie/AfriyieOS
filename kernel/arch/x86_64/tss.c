// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 Task State Segment
//
// =============================================================================
// WHAT THE TSS IS ACTUALLY FOR, IN 64-BIT MODE
// =============================================================================
// Everything hardware task switching was used for in 32-bit mode is obsolete —
// the kernel switches stacks itself. But one field is not, and it is not
// optional:
//
//   RSP0 — the stack pointer the CPU loads when an interrupt or exception
//          arrives while the processor is in a LESS privileged ring.
//
// Without it, an interrupt taken in ring 3 loads the USER stack pointer into the
// kernel's stack register. The handler then pushes its frame — return address,
// saved registers, the lot — onto a stack the user process owns and can
// overwrite. The process can therefore choose what the kernel's exception
// handler returns to, which is not a bug, it is a privilege escalation.
//
// So the TSS is not a legacy curiosity here. It is the mechanism that makes the
// kernel/user boundary survivable under an interrupt, and a ring-3 transition
// without it faults immediately in a way that looks like anything but a missing
// stack.
// =============================================================================

#include "x86_64.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

// The 64-bit TSS layout. The packed attribute matters: the reserved field after
// the three stack pointers exists precisely to keep IST 8-byte aligned, and a
// compiler that removes it would shift every field after it.
typedef struct AF_PACKED {
    af_u32 reserved0;
    af_u64 rsp0;            // stack for interrupts arriving from ring 3
    af_u64 rsp1;
    af_u64 rsp2;
    af_u64 reserved1;
    af_u64 ist[7];          // interrupt stack table
    af_u64 reserved2;
    af_u16 reserved3;
    af_u16 iomap_base;      // offset of the I/O permission bitmap
} af_tss_t;

// 4 + 24 + 8 + 56 + 8 + 2 + 2 = 104 bytes.
AF_STATIC_ASSERT_SIZE(af_tss_t, 104);
AF_STATIC_ASSERT_OFFSET(af_tss_t, rsp0, 4);
AF_STATIC_ASSERT_OFFSET(af_tss_t, ist, 36);

static af_tss_t s_tss __attribute__((aligned(16)));

// A dedicated stack for kernel entry from user mode.
//
// Separate from the boot stack on purpose. Once user processes exist, every one
// of them will need its own entry stack, and reaching that point by sharing the
// boot stack would mean the current design gives no hint of where the split
// belongs. One dedicated stack now makes the shape of the eventual change
// obvious rather than requiring a redesign.
#define AF_SYSCALL_STACK_SIZE (16 * AF_KIB)
static af_u8 s_syscall_stack[AF_SYSCALL_STACK_SIZE] __attribute__((aligned(16)));

// -----------------------------------------------------------------------------
// Building the GDT descriptor for the TSS
// -----------------------------------------------------------------------------
//
// A system descriptor in long mode is SIXTEEN bytes: it uses two consecutive
// GDT slots. The base is split across three fields and the limit across two, and
// the upper half is mostly zeroes. Writing a single ordinary 8-byte descriptor —
// which is what a code or data segment needs — produces a TSS the CPU loads
// incorrectly, and `ltr` then faults with a #GP whose error code points at the
// selector rather than at the descriptor.
static void write_tss_descriptor(af_u64 base, af_u32 limit)
{
    // Index 5, occupying slots 5 and 6.
    af_u64 low = 0;
    af_u64 high = 0;

    low |= (af_u64)(limit & 0xFFFF);                  // limit 15:0
    low |= (af_u64)(base & 0xFFFF) << 16;             // base 15:0
    low |= (af_u64)((base >> 16) & 0xFF) << 32;       // base 23:16
    low |= (af_u64)0x89 << 40;                        // present, type = 64-bit TSS (available)
    low |= (af_u64)((limit >> 16) & 0x0F) << 48;      // limit 19:16
    low |= (af_u64)((base >> 24) & 0xFF) << 56;       // base 31:24

    high |= (af_u64)((base >> 32) & 0xFFFFFFFF);      // base 63:32

    // The descriptor lives in the same GDT the C file builds, at slot 5.
    // Declared in x86_64.h alongside the rest of the GDT interface.
    af_x86_gdt_write_raw(5, low, high);
}

void af_x86_tss_init(void)
{
    af_memset(&s_tss, 0, sizeof(s_tss));

    // The CPU writes this value into RSP when it enters the kernel from ring 3.
    // A stack grows DOWN, so the initial value is the TOP of the region.
    s_tss.rsp0 = (af_u64)(af_uptr)(s_syscall_stack + AF_SYSCALL_STACK_SIZE);

    // IST entries are left at zero: nothing uses an interrupt stack table yet.
    // The double-fault handler is the one that genuinely needs it (a #DF taken
    // on a bad stack cannot report anything), and that is a v0.5 task.

    s_tss.iomap_base = sizeof(af_tss_t);

    write_tss_descriptor((af_u64)(af_uptr)&s_tss, (af_u32)(sizeof(s_tss) - 1));

    // Load the task register. `ltr` takes the SELECTOR, not the address.
    //
    // Note that ltr can only be executed once per CPU with a given descriptor
    // until a task switch marks it not-busy: our descriptor has the "available"
    // type (0x9) precisely so that it is not marked busy and a second ltr is not
    // rejected with #GP.
    __asm__ __volatile__("ltr %0" :: "rm"((af_u16)AF_GDT_TSS));

    af_info("tss", "TSS installed at %p: RSP0 = 0x%lX (ring-3 entry stack, "
                   "%u KiB)",
            (void *)&s_tss, (af_u64)s_tss.rsp0,
            (af_u32)(AF_SYSCALL_STACK_SIZE / AF_KIB));
}

// -----------------------------------------------------------------------------
// Updating RSP0
//
// Called on every switch into a thread that has user-mode work, so that an
// interrupt arriving in ring 3 lands on THAT task's kernel stack rather than on
// whichever stack happened to be installed. v0.4 has a single ring-3 task and
// therefore a single entry stack; the per-thread version arrives with processes.
// -----------------------------------------------------------------------------
void af_x86_tss_set_rsp0(af_u64 rsp0)
{
    s_tss.rsp0 = rsp0;
}

af_u64 af_x86_tss_get_rsp0(void)
{
    return s_tss.rsp0;
}

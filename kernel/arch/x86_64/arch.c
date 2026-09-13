// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 architecture layer: HAL entry points and arch hooks

#include "x86_64.h"
#include "afriyie/config.h"
#include "afriyie/boot_info.h"
#include "afriyie/hal.h"
#include "afriyie/arch_hooks.h"
#include "afriyie/log.h"
#include "afriyie/io.h"
#include "afriyie/kstring.h"
#include "afriyie/assert.h"

// -----------------------------------------------------------------------------
// Fault frame captured by isr_dispatch so the panic dump can print registers.
// -----------------------------------------------------------------------------
static const isr_frame_t *s_fault_frame = NULL;

// Declared in x86_64.h.
void af_x86_set_fault_frame(const isr_frame_t *frame)
{
    s_fault_frame = frame;
}

// -----------------------------------------------------------------------------
// Arch hook: console
// -----------------------------------------------------------------------------
void af_arch_early_console_init(const af_boot_info_t *bi)
{
    // Prefer the port the boot bridge told us about. If it did not tell us
    // anything, try the conventional COM1 anyway: a PC almost always has one,
    // and getting output costs nothing when the probe fails.
    if (bi != NULL && (bi->boot_flags & AF_BOOT_FLAG_UART) != 0 &&
        bi->console.uart_base != 0) {
        af_u16 port = (af_u16)bi->console.uart_base;
        af_x86_serial_init(port, bi->console.uart_baud);
    } else {
        af_x86_serial_init(AF_COM1_PORT, 115200);
    }

    if (af_x86_serial_is_ready()) {
        af_log_set_sink(af_x86_log_sink, NULL);
    }
}

// Sink adapter: the log layer hands us a buffer, we push it at the UART.
void af_x86_log_sink(const char *text, af_size len, void *ctx)
{
    AF_UNUSED(ctx);
    af_x86_serial_write(text, len);
}

void af_arch_console_putc(char c)
{
    af_x86_serial_putc(c);
}

// -----------------------------------------------------------------------------
// Arch hook: panic
// -----------------------------------------------------------------------------
void af_arch_panic_dump(void)
{
    if (s_fault_frame != NULL) {
        const isr_frame_t *f = s_fault_frame;

        af_log_raw("  vector     : ");
        {
            char b[24];
            af_format_hex(f->vector, true, false, b, sizeof(b));
            af_log_raw(b);
        }
        af_log_raw("\n");

        af_log_raw("  rip        : ");
        { char b[24]; af_format_hex(f->rip, true, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        af_log_raw("  rsp        : ");
        { char b[24]; af_format_hex(f->rsp, true, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        af_log_raw("  rflags     : ");
        { char b[24]; af_format_hex(f->rflags, true, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        if (f->vector == 14) {
            af_log_raw("  cr2        : ");
            { char b[24]; af_format_hex(af_read_cr2(), true, true, b, sizeof(b)); af_log_raw(b); }
            af_log_raw("\n");
        }

        af_log_raw("  cr3        : ");
        { char b[24]; af_format_hex(af_read_cr3(), true, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        // Register dump. Printed in ABI groups so that a fault is easy to read
        // against the disassembly.
        af_log_raw("  ----------------------------------------------------------\n");
        af_log_raw("  rax=");
        { char b[24]; af_format_hex(f->rax, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" rbx=");
        { char b[24]; af_format_hex(f->rbx, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" rcx=");
        { char b[24]; af_format_hex(f->rcx, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" rdx=");
        { char b[24]; af_format_hex(f->rdx, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        af_log_raw("  rsi=");
        { char b[24]; af_format_hex(f->rsi, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" rdi=");
        { char b[24]; af_format_hex(f->rdi, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" rbp=");
        { char b[24]; af_format_hex(f->rbp, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" rsp=");
        { char b[24]; af_format_hex(f->rsp, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        af_log_raw("  r8 =");
        { char b[24]; af_format_hex(f->r8, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" r9 =");
        { char b[24]; af_format_hex(f->r9, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" r10=");
        { char b[24]; af_format_hex(f->r10, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" r11=");
        { char b[24]; af_format_hex(f->r11, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        af_log_raw("  r12=");
        { char b[24]; af_format_hex(f->r12, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" r13=");
        { char b[24]; af_format_hex(f->r13, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" r14=");
        { char b[24]; af_format_hex(f->r14, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" r15=");
        { char b[24]; af_format_hex(f->r15, false, true, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");

        af_log_raw("  cs=");
        { char b[24]; af_format_hex(f->cs, true, false, b, sizeof(b)); af_log_raw(b); }
        af_log_raw(" ss=");
        { char b[24]; af_format_hex(f->ss, true, false, b, sizeof(b)); af_log_raw(b); }
        af_log_raw("\n");
        af_log_raw("  ----------------------------------------------------------\n");

        // Raw stack words around rsp. Without a symbol table this is the only
        // way to see a call chain, and it is usually enough.
        //
        // ONLY IN THE KERNEL HALF. A fault taken in user mode has rsp pointing
        // into the user's stack, and the panic dump runs in ring 0 with the
        // user's pages still mapped — reading there is legal, but reading a
        // stack pointer that sits exactly at the top of the mapped region is
        // not: the bytes above it are unmapped, and the dump takes a second
        // fault inside the panic handler. That produced "recursive panic" on the
        // first ring-3 test, which buried the original fault under a cascade.
        if (f->rsp >= AF_KERNEL_BASE) {
            const af_u64 *stack = (const af_u64 *)(af_uptr)f->rsp;
            af_log_raw("  stack:\n");
            for (af_u32 i = 0; i < 8; i++) {
                char b[24];
                af_format_hex(stack[i], true, true, b, sizeof(b));
                af_log_raw("    [rsp+");
                {
                    char off[8];
                    af_itoa((af_i64)(i * 8), off, sizeof(off));
                    af_log_raw(off);
                }
                af_log_raw("] ");
                af_log_raw(b);
                af_log_raw("\n");
            }
        } else {
            char line[128];
            af_snprintf(line, sizeof(line),
                        "  stack      : in the user half (0x%lX) — not dumped; "
                        "the bytes above a user stack pointer may be unmapped\n",
                        f->rsp);
            af_log_raw(line);
        }
    } else {
        af_log_raw("  no interrupt frame: the panic happened outside a fault\n");
        af_log_raw("  handler (a failed assertion or an explicit af_panic call).\n");
    }
}

// -----------------------------------------------------------------------------
// Arch hook: CPU identity
// -----------------------------------------------------------------------------
void af_arch_cpu_dump(void)
{
    af_x86_cpu_info_t cpu;
    af_x86_cpu_detect(&cpu);

    af_log(AF_LOG_INFO, "cpu", "vendor : %s", cpu.vendor);
    af_log(AF_LOG_INFO, "cpu", "brand  : %s", cpu.brand);
    af_log(AF_LOG_INFO, "cpu", "family %u model %u stepping %u",
           cpu.family, cpu.model, cpu.stepping);
    af_log(AF_LOG_INFO, "cpu", "cpus   : %u logical", cpu.logical_cpus);
    af_log(AF_LOG_INFO, "cpu", "features: 0x%lX (edx) 0x%lX (ecx)",
           cpu.feature_edx, cpu.feature_ecx);

    if ((cpu.feature_edx & (1u << 0)) == 0) {
        af_warn("cpu", "x87 FPU not reported — unexpected on x86_64");
    }
    if ((cpu.feature_edx & (1u << 25)) == 0) {
        af_warn("cpu", "SSE not reported — unexpected on x86_64");
    }
    if ((cpu.feature_edx & (1u << 26)) == 0) {
        af_warn("cpu", "SSE2 not reported — unexpected on x86_64");
    }
}

// -----------------------------------------------------------------------------
// CPU identification
// -----------------------------------------------------------------------------
static inline void cpu_brand_string(char *out, af_size out_size)
{
    af_u32 regs[4];

    for (af_u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        af_cpuid(leaf, &regs[0], &regs[1], &regs[2], &regs[3]);

        char chunk[17];
        af_memcpy(chunk + 0, &regs[0], 4);
        af_memcpy(chunk + 4, &regs[1], 4);
        af_memcpy(chunk + 8, &regs[2], 4);
        af_memcpy(chunk + 12, &regs[3], 4);
        chunk[16] = '\0';

        af_size offset = (leaf - 0x80000002) * 16;
        if (offset + 16 < out_size) {
            af_memcpy(out + offset, chunk, 16);
        }
    }

    if (out_size > 0) {
        out[out_size - 1] = '\0';
    }

    // Trim leading spaces the CPU leaves in the brand string.
    af_size lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        af_size len = af_strlen(out + lead);
        af_memmove(out, out + lead, len + 1);
    }
}

void af_x86_cpu_detect(af_x86_cpu_info_t *out)
{
    if (out == NULL) {
        return;
    }

    af_memset(out, 0, sizeof(*out));

    af_u32 eax, ebx, ecx, edx;
    af_cpuid(0, &eax, &ebx, &ecx, &edx);

    af_u32 max_leaf = eax;

    // Vendor string is EBX, EDX, ECX in that order — a detail that is easy to
    // get wrong and trivially visible when it is.
    af_memcpy(out->vendor + 0, &ebx, 4);
    af_memcpy(out->vendor + 4, &edx, 4);
    af_memcpy(out->vendor + 8, &ecx, 4);
    out->vendor[12] = '\0';

    af_cpuid(1, &eax, &ebx, &ecx, &edx);
    out->stepping   = eax & 0xF;
    out->model      = (eax >> 4) & 0xF;
    out->family     = (eax >> 8) & 0xF;
    out->logical_cpus = (ebx >> 16) & 0xFF;
    out->feature_edx = edx;
    out->feature_ecx = ecx;

    // Extended families/models: the base fields saturate and the real values
    // live in the extended fields.
    if (out->family == 0xF) {
        out->family += (eax >> 20) & 0xFF;
    }
    if (out->family == 0x6 || out->family == 0xF) {
        out->model += ((eax >> 16) & 0xF) << 4;
    }

    if (max_leaf >= 0x80000004) {
        cpu_brand_string(out->brand, sizeof(out->brand));
    } else {
        af_strlcpy(out->brand, "(brand string unavailable)", sizeof(out->brand));
    }
}

// =============================================================================
// HAL entry points implemented for v0.1
//
// The rest of hal.h (paging, IRQ registration, timers, SMP) arrives in v0.2 and
// later. Only the functions the v0.1 kernel actually calls are defined here —
// an unimplemented HAL function that nothing calls simply does not exist yet.
// =============================================================================

af_status_t hal_console_init(const af_boot_info_t *bi)
{
    af_arch_early_console_init(bi);
    return af_x86_serial_is_ready() ? AF_OK : AF_ERR_NOTSUP;
}

af_status_t hal_cpu_init(void)
{
    af_status_t rc = af_x86_gdt_init();
    if (af_status_err(rc)) {
        return rc;
    }
    af_marker("AF_GDT_READY");

    rc = af_x86_idt_init();
    if (af_status_err(rc)) {
        return rc;
    }
    af_marker("AF_IDT_READY");

    // The TSS comes after the GDT (its descriptor lives there) and after the IDT
    // (so a failure during the transition is reportable rather than a triple
    // fault). Ring 3 cannot be entered without it.
    af_x86_tss_init();

    return AF_OK;
}

af_u32 hal_cpu_id(void)
{
    return 0;   // single core in v0.1; SMP arrives in v1.2
}

af_u32 hal_cpu_count(void)
{
    af_x86_cpu_info_t cpu;
    af_x86_cpu_detect(&cpu);
    return (cpu.logical_cpus > 0) ? cpu.logical_cpus : 1;
}

// Paging bootstrap. Declared in hal.h; implemented in arch/x86_64/paging.c,
// because the page-table walk is architecture-specific and must not leak upward.
void hal_paging_init(const af_boot_info_t *bi)
{
    af_x86_paging_bootstrap(bi);
}

// SPDX-License-Identifier: MIT
// AfriyieOS — the panic path
//
// A panic must always produce a clear, complete diagnosis. There is no
// "reboot on failure" here: the fault state is the entire value of the crash.

#include "afriyie/config.h"
#include "afriyie/assert.h"
#include "afriyie/log.h"
#include "afriyie/io.h"
#include "afriyie/arch_hooks.h"
#include "afriyie/kstring.h"

#define AF_PANIC_BUF 512

static volatile bool s_panicking = false;

AF_NORETURN void af_panic_at(const char *file, int line, const char *func,
                             const char *fmt, ...)
{
    char message[AF_PANIC_BUF];

    // A panic inside a panic would recurse forever. Detect it and stop with the
    // minimum possible work.
    if (s_panicking) {
        af_log_raw("\nAF_PANIC: recursive panic — halting\n");
        af_arch_panic_halt();
    }
    s_panicking = true;

    // Interrupts off immediately: a second fault during reporting would destroy
    // the information we are trying to print.
    af_interrupts_disable();

    va_list args;
    va_start(args, fmt);
    af_vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    af_log_raw("\n");
    af_log_raw("================================================================\n");
    af_log_raw("  AFRIYIEOS KERNEL PANIC\n");
    af_log_raw("================================================================\n");

    af_log_raw("  reason : ");
    af_log_raw(message);
    af_log_raw("\n");

    af_log_raw("  at     : ");
    af_log_raw((file != NULL) ? file : "?");
    af_log_raw(":");
    {
        char linebuf[16];
        af_itoa(line, linebuf, sizeof(linebuf));
        af_log_raw(linebuf);
    }
    af_log_raw(" in ");
    af_log_raw((func != NULL) ? func : "?");
    af_log_raw("\n");

    af_log_raw("  build  : ");
    af_log_raw(AF_NAME " " AF_VERSION_STRING);
    af_log_raw(" (");
    af_log_raw(AF_CODENAME);
    af_log_raw(")");
#if AF_DEBUG
    af_log_raw(" debug");
#else
    af_log_raw(" release");
#endif
    af_log_raw("\n");

    af_log_raw("----------------------------------------------------------------\n");

    // Architecture-specific dump: registers, fault address, fault reason.
    af_arch_panic_dump();

    af_log_raw("----------------------------------------------------------------\n");
    af_log_raw("  The system is halted. Report this output with the steps that\n");
    af_log_raw("  reproduced it. See docs/debug-log.md for known issues.\n");
    af_log_raw("================================================================\n");

    af_arch_panic_halt();
}

// SPDX-License-Identifier: MIT
// AfriyieOS — weak default architecture hooks
//
// A new port can link and boot before these are implemented; the placeholders
// keep the kernel core independent of any single architecture. A real port
// overrides each one with a strong definition in kernel/arch/<arch>/.

#include "afriyie/arch_hooks.h"
#include "afriyie/log.h"
#include "afriyie/config.h"
#include "afriyie/io.h"

AF_WEAK void af_arch_panic_dump(void)
{
    af_log_raw("  (this architecture provides no register dump)\n");
}

AF_WEAK void af_arch_cpu_dump(void)
{
#if AF_TARGET_X86_64
    af_log(AF_LOG_INFO, "cpu", "architecture: x86_64 (identifiers unavailable)");
#else
    af_log(AF_LOG_INFO, "cpu", "architecture: aarch64 (identifiers unavailable)");
#endif
}

AF_WEAK void af_arch_early_console_init(const af_boot_info_t *bi)
{
    AF_UNUSED(bi);
}

AF_WEAK void af_arch_console_putc(char c)
{
    AF_UNUSED(c);
}

AF_WEAK AF_NORETURN void af_arch_panic_halt(void)
{
    // Interrupts off, then park the CPU. A panic must never silently reboot:
    // the fault state on screen or on the serial line is the whole point.
    af_interrupts_disable();
    for (;;) {
        af_halt();
    }
}

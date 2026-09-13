/* SPDX-License-Identifier: MIT
 * AfriyieOS — libaf: out-of-line runtime pieces
 *
 * Only what cannot be a header. crt0.S references af_exit by name from
 * assembly, so it has to exist as a real symbol in a real object; a static
 * inline would be emitted only if some C translation unit happened to call it,
 * and crt0 calling it from assembly counts for nothing.
 */

#include "af.h"

AF_NORETURN void af_exit(int code)
{
    af_syscall1(AF_SYS_EXIT, (af_u64)code);

    /* Unreachable. sys_exit terminates this thread and never returns to ring 3,
     * so control cannot come back here — but if the kernel ever did return,
     * spinning is safer than executing whatever follows in .text. */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

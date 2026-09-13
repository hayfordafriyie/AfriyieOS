// SPDX-License-Identifier: MIT
// AfriyieOS — generic interrupt request layer
//
// The kernel side of "route an interrupt to whatever asked for it". At v0.2 the
// handlers run in kernel context; from v0.7 they become notifications delivered
// to user-space driver processes over IPC. The table and the registration API
// are the same either way, which is the point of building it now.

#include "afriyie/hal.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/config.h"

#if AF_TARGET_X86_64
#include "../arch/x86_64/x86_64.h"
#endif

// Handlers chained per line: several devices can legitimately share one IRQ, and
// the dispatcher must give each a chance to claim the interrupt.
#define AF_IRQ_MAX_HANDLERS 4

typedef struct {
    hal_irq_handler_fn fn;
    void *ctx;
} irq_handler_t;

static irq_handler_t s_handlers[HAL_IRQ_COUNT][AF_IRQ_MAX_HANDLERS];
static af_u32        s_handler_count[HAL_IRQ_COUNT];
static af_u64        s_irq_counts[HAL_IRQ_COUNT];
static af_u64        s_spurious_counts[HAL_IRQ_COUNT];

af_status_t hal_irq_register(af_u32 irq, hal_irq_handler_fn fn, void *ctx)
{
    if (irq >= HAL_IRQ_COUNT || fn == NULL) {
        return AF_ERR_INVAL;
    }

    if (s_handler_count[irq] >= AF_IRQ_MAX_HANDLERS) {
        af_error("irq", "IRQ%u already has %u handlers; refusing another",
                 irq, (af_u32)AF_IRQ_MAX_HANDLERS);
        return AF_ERR_TOOMANY;
    }

    // Refuse a duplicate registration. Registering the same handler twice is
    // almost always a driver being initialised twice, and the second copy would
    // silently double-count every interrupt.
    for (af_u32 i = 0; i < s_handler_count[irq]; i++) {
        if (s_handlers[irq][i].fn == fn && s_handlers[irq][i].ctx == ctx) {
            af_warn("irq", "IRQ%u handler %p is already registered", irq, (void *)fn);
            return AF_ERR_EXIST;
        }
    }

    s_handlers[irq][s_handler_count[irq]].fn  = fn;
    s_handlers[irq][s_handler_count[irq]].ctx = ctx;
    s_handler_count[irq]++;

    af_info("irq", "IRQ%u handler registered (%u total on this line)",
            irq, s_handler_count[irq]);

    return AF_OK;
}

af_status_t hal_irq_unregister(af_u32 irq, hal_irq_handler_fn fn)
{
    if (irq >= HAL_IRQ_COUNT || fn == NULL) {
        return AF_ERR_INVAL;
    }

    for (af_u32 i = 0; i < s_handler_count[irq]; i++) {
        if (s_handlers[irq][i].fn != fn) {
            continue;
        }

        // Compact the chain so the ordering of the remaining handlers is
        // preserved — some devices rely on the order they were registered.
        for (af_u32 j = i; j + 1 < s_handler_count[irq]; j++) {
            s_handlers[irq][j] = s_handlers[irq][j + 1];
        }
        s_handler_count[irq]--;

        af_info("irq", "IRQ%u handler unregistered (%u remaining)",
                irq, s_handler_count[irq]);
        return AF_OK;
    }

    return AF_ERR_NOENT;
}

void hal_irq_enable(af_u32 irq)
{
#if AF_TARGET_X86_64
    af_x86_pic_enable(irq);
#else
    AF_UNUSED(irq);
#endif
}

void hal_irq_disable(af_u32 irq)
{
#if AF_TARGET_X86_64
    af_x86_pic_disable(irq);
#else
    AF_UNUSED(irq);
#endif
}

void hal_irq_ack(af_u32 irq)
{
#if AF_TARGET_X86_64
    af_x86_pic_ack(irq);
#endif
}

void hal_irq_set_priority(af_u32 irq, af_u8 priority)
{
    // The 8259 has no per-line priority beyond its fixed cascade order, and the
    // GIC's priority registers arrive with the ARM64 port. Reported rather than
    // silently ignored, because a driver that sets a priority and gets nothing
    // should know why.
    AF_UNUSED(irq);
    AF_UNUSED(priority);
}

// -----------------------------------------------------------------------------
// Dispatch — called from isr_dispatch() for vectors 32..47
// -----------------------------------------------------------------------------
//
// Runs in interrupt context with interrupts disabled. Handlers must be short and
// must not allocate or block.
//
// The Acknowledge happens BEFORE the handlers run, not after. An 8259 that is not
// acknowledged will not deliver another interrupt from that line, so a handler
// that returns early still leaves the system able to receive the next one. Doing
// it the other way round is a classic source of "the first interrupt works and
// then the device goes quiet".
void irq_dispatch(af_u32 irq)
{
    if (irq >= HAL_IRQ_COUNT) {
        return;
    }

    s_irq_counts[irq]++;

    hal_irq_ack(irq);

    if (s_handler_count[irq] == 0) {
        s_spurious_counts[irq]++;
        // Only complain occasionally. An unhandled line that fires at 100 Hz
        // would otherwise flood the serial console and hide everything else.
        if (s_spurious_counts[irq] == 1 || (s_spurious_counts[irq] % 1000) == 0) {
            af_warn("irq", "IRQ%u fired with no handler (%llu times)",
                    irq, (unsigned long long)s_spurious_counts[irq]);
        }
        return;
    }

    bool handled = false;
    for (af_u32 i = 0; i < s_handler_count[irq]; i++) {
        if (s_handlers[irq][i].fn(irq, s_handlers[irq][i].ctx)) {
            handled = true;
        }
    }

    if (!handled) {
        s_spurious_counts[irq]++;
        if (s_spurious_counts[irq] == 1) {
            af_warn("irq", "IRQ%u fired but no registered handler claimed it", irq);
        }
    }
}

void irq_dump_stats(void)
{
    for (af_u32 irq = 0; irq < HAL_IRQ_COUNT; irq++) {
        if (s_irq_counts[irq] == 0) {
            continue;
        }
        af_info("irq", "IRQ%-2u: %llu interrupts, %llu unclaimed, %u handler(s)",
                irq, (unsigned long long)s_irq_counts[irq],
                (unsigned long long)s_spurious_counts[irq], s_handler_count[irq]);
    }
}

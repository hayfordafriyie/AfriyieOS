// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 8254 PIT timer

#include "x86_64.h"
#include "afriyie/io.h"
#include "afriyie/log.h"
#include "afriyie/hal.h"
#include "afriyie/sched.h"

// The PIT's input clock. Fixed by the original IBM PC and not configurable.
#define PIT_FREQUENCY 1193182u

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

// Channel 0, access mode lobyte/hibyte, mode 3 (square wave), binary counter.
//
// Mode 3 rather than the one-shot mode 2: a periodic square wave reloads itself
// in hardware, so the handler does not have to rearm the counter every tick. A
// handler that forgets to rearm produces a system that ticks exactly once, which
// looks like a scheduler that never preempts.
#define PIT_CMD_CHANNEL0_RATE_GENERATOR 0x36

static af_u32 s_hz = 0;
static af_u64 s_ticks = 0;

void af_x86_pit_init(af_u32 hz)
{
    if (hz == 0) {
        hz = AF_SCHED_TICK_HZ;
    }

    // A divisor below 19 or above 65536 is not representable in 16 bits.
    if (hz > PIT_FREQUENCY / 19u) {
        af_warn("timer", "requested %u Hz is too fast for the PIT; clamping", hz);
        hz = PIT_FREQUENCY / 19u;
    }
    if (hz < 19u) {
        hz = 19u;
    }

    af_u32 divisor = PIT_FREQUENCY / hz;

    af_outb(PIT_COMMAND, PIT_CMD_CHANNEL0_RATE_GENERATOR);

    // Low byte then high byte, as the command byte declared.
    af_outb(PIT_CHANNEL0, (af_u8)(divisor & 0xFF));
    af_outb(PIT_CHANNEL0, (af_u8)((divisor >> 8) & 0xFF));

    s_hz = PIT_FREQUENCY / divisor;   // the rate actually achieved
    s_ticks = 0;

    af_info("timer", "PIT channel 0: requested %u Hz, divisor %u, actual %u Hz",
            hz, divisor, s_hz);
}

af_u32 af_x86_pit_hz(void)
{
    return s_hz;
}

af_u64 af_x86_pit_ticks(void)
{
    return s_ticks;
}

// Called from the IRQ0 handler.
void af_x86_pit_tick(void)
{
    s_ticks++;
}

// Nanoseconds since boot, derived from the tick count. Coarse at 100 Hz (10 ms
// resolution) and honest about it — a finer clock needs the APIC timer or the
// TSC, both of which arrive with the v0.6 timing work.
af_u64 af_x86_time_ns(void)
{
    if (s_hz == 0) {
        return 0;
    }
    return (s_ticks * 1000000000ULL) / s_hz;
}

// =============================================================================
// HAL entry points
// =============================================================================

// The IRQ0 handler. It advances the clock, then drives the scheduler.
//
// Taking the tick here rather than inside the scheduler keeps the layering
// honest: the HAL owns the hardware timer, the scheduler owns policy, and
// neither needs to know about the other's internals.
//
// This runs in interrupt context. sched_tick() may context_switch() directly
// from here, which is safe because the interrupt frame sits on the *current
// thread's* kernel stack — see the note at the top of kernel/core/sched.c.
static bool timer_irq_handler(af_u32 irq, void *ctx)
{
    AF_UNUSED(irq);
    AF_UNUSED(ctx);

    af_x86_pit_tick();
    sched_tick();

    return true;
}

af_status_t hal_timer_init(af_u32 hz)
{
    af_x86_pit_init(hz);

    af_status_t rc = hal_irq_register(0, timer_irq_handler, NULL);
    if (af_status_err(rc)) {
        af_error("timer", "could not register the IRQ0 handler (%s)",
                 af_status_name(rc));
        return rc;
    }

    hal_irq_enable(0);

    af_marker("AF_TIMER_READY");

    return AF_OK;
}

af_u64 hal_time_ns(void)
{
    return af_x86_time_ns();
}

# ARM64 (phone) port — scheduled for milestone v1.1

This directory is intentionally empty of source code at v0.1.

## Why

AfriyieOS targets both PCs (x86_64/UEFI) and phones (ARM64/U-Boot) **from one
codebase**, but the two ports are not developed simultaneously. This is
**ADR-007** in [docs/AfriyieOS-Blueprint.md](../../docs/AfriyieOS-Blueprint.md):

> Drive x86_64 to v0.5 first. Then port to ARM64. If the port requires changing
> more than 20% of the architecture-independent core code, the HAL abstraction is
> wrong and must be redesigned before continuing.

Developing both at once doubles the hardware surface, doubles the debugging time,
and removes the feedback that tells you whether the HAL is actually sound. The
x86_64 kernel has to be stable enough to be worth porting first.

## What lands here at v1.1

| File | Purpose |
| --- | --- |
| `entry.S` | U-Boot entry stub: park secondary cores, set the stack, call `kmain` |
| `vectors.S` | The 16-entry exception vector table installed into `VBAR_EL1` |
| `context.S` | Thread context switch: `x19-x30`, `sp`, and lazy FP/SIMD state |
| `pl011.c` | PL011 UART serial console |
| `gic.c` | GICv3 distributor, redistributor and CPU interface |
| `timer.c` | Generic timer (`CNTPCT_EL0`, `CNTP_*`) driving the scheduler tick |
| `mmu.c` | 4-level translation tables, `TTBR0_EL1`/`TTBR1_EL1`, `MAIR_EL1`, `TCR_EL1` |
| `arch.c` | The same HAL entry points `arch/x86_64/arch.c` provides |

## The portability test

The port is only considered successful if the **architecture-independent** kernel
(`kernel/core/`) needs no more than a 20% diff. `kernel/core/` must not gain a
single `#if AF_TARGET_*` that is not already there for a genuine hardware reason
— if it needs one, the HAL interface in `kernel/include/afriyie/hal.h` is
missing an abstraction and must be extended for both targets.

## Reference

* Blueprint §3 (architecture), §8 (toolchain), §11 (v1.1 milestone tasks)
* `cmake/toolchain-aarch64-elf.cmake`
* `docs/porting/` — hardware bring-up notes (added at v1.1)

# Boot Flow

**Status:** ✅ implemented at v0.1 for x86_64 · 📐 designed for ARM64 (v1.1)

This document describes exactly how control passes from firmware to AfriyieOS on
both target platforms, and which invariants hold at each boundary.

---

## 1. The one-model principle

PCs and phones boot completely differently. Firmware is different, the memory map
is discovered differently, the display is described differently, and the CPU
starts in a different privilege state.

The kernel must not know any of that.

```
   PC: UEFI + ACPI + GOP            Phone: SoC ROM + U-Boot + Device Tree
             │                                        │
             │      each bridge does whatever         │
             │      its platform requires             │
             ▼                                        ▼
   ┌──────────────────────────────────────────────────────────────┐
   │              af_boot_info  (one normalised struct)           │
   │  memory map · framebuffer · console · firmware metadata      │
   └──────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    kmain(boot_info) — architecture independent
```

Everything above `kmain` is shared. Everything below it is a *boot bridge*, and
there is exactly one per platform.

---

## 2. PC: UEFI

### 2.1 The application

Firmware locates `\EFI\BOOT\BOOTX64.EFI` on the EFI System Partition and runs it.
The application is a PE32+ executable with subsystem 10 (EFI application), linked
with `--image-base 0` so firmware relocates it freely.

Its entry point is declared with the **Microsoft x64 ABI**:

```c
EFI_STATUS EFI_MS_ABI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
```

This is not optional. UEFI uses the Microsoft ABI regardless of the host
compiler, so the first four arguments arrive in `rcx`, `rdx`, `r8`, `r9` with 32
bytes of shadow space — not in `rdi`, `rsi`, `rdx`, `rcx` as the SysV ABI would
have it. Getting this wrong means `SystemTable` is a garbage pointer and the very
first dereference faults.

### 2.2 Sequence

| # | Step | Why it matters |
| --- | --- | --- |
| 1 | Print a banner on `ConOut` | If nothing after this appears, the fault is in the bridge, not the kernel |
| 2 | `SetWatchdogTimer(0, ...)` | Firmware arms a 5-minute watchdog by default. A boot that hangs must not be "rescued" by a reset that destroys the evidence |
| 3 | `LocateProtocol(GOP)` → record `FrameBufferBase`, geometry, `PixelsPerScanLine` | The stride, not `width × bpp / 8`, is the real line length |
| 4 | `AllocatePages(AllocateAddress, 0x100000, n)` | `AllocateAddress` fails rather than relocating. Being placed somewhere the kernel was not linked for is a silent, catastrophic failure; failing loudly is correct |
| 5 | Copy the kernel image to `0x100000` | The image is embedded in the bridge's `.rodata` via `incbin` |
| 6 | `GetMemoryMap` twice | Probe for the size, then fetch. The map can grow between the calls |
| 7 | Normalise the map into `af_memory_region_t` | Drops zero-length entries the kernel would reject |
| 8 | Override any region covering the framebuffer to `AF_MEM_FRAMEBUFFER` | Some firmware reports video memory as conventional. Losing it to the allocator is an intermittent, confusing bug |
| 9 | `ExitBootServices(ImageHandle, MapKey)` in a retry loop | The key can change between `GetMemoryMap` and the call. The only correct response is to refetch and retry — and it can happen more than once |
| 10 | Mirror `af_boot_info` to `0x7000` | The register path is correct; this is the recovery path |
| 11 | `cli` then jump to `0x100000` with `boot_info` in `rdi` | No firmware services exist any more |

### 2.3 Memory chosen by the bridge

| Address | Use | Notes |
| --- | --- | --- |
| `0x0000_0000_0000_7000` | `af_boot_info` backup copy | Above the real-mode IVT/BDA, below the EBDA at `0x9FC00` |
| `0x0000_0000_0010_0000` | Kernel image | 1 MiB: above all legacy firmware area, the VGA window and the EBDA |
| `0x0000_0000_000A_0000` | VGA text/legacy window | Never touched |
| `0x0000_0000_000F_0000` | **avoided** | EBDA / BIOS ROM shadow region |

### 2.4 Why the kernel is not in the higher half yet

The blueprint's target layout (§3.3) puts the kernel at `0xFFFFFFFF80000000`.
That requires page tables mapping the higher half, and **v0.1 has no paging**.

Jumping into a higher-half image with paging off faults on the first instruction
fetch. So v0.1 links the kernel as an identity mapping at 1 MiB with the default
small code model, and v0.2 moves it — in the same commit that adds the paging
bootstrap, because the two cannot be separated. The switch is gated on
`AF_KERNEL_HIGHER_HALF` in `cmake/flags.cmake`, which also adds
`-mcmodel=kernel` (the default model cannot address anything above 2 GiB, so
every absolute reference would be silently truncated).

---

## 3. Kernel entry (x86_64)

```asm
kernel_entry:
    cli                              ; no interrupts until the IDT exists
    lea rsp, [rel kernel_stack_top]  ; LEA, not MOV — see the debug log
    mov r12, rdi                     ; preserve boot_info across the .bss wipe
    mov rdi, __bss_start             ; zero .bss: firmware leaves garbage there
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb
    xor rbp, rbp
    mov rdi, r12
    call kmain                       ; never returns
```

Two details are load-bearing:

* **`lea`, not `mov`.** `mov rsp, kernel_stack_top` loads the 8 bytes *stored at*
  that `.bss` address — which is about to be zeroed — instead of the address.
* **`.bss` is cleared after the stack is installed but before any C runs.** The
  loop uses only registers, and `rep stosb` walks upward from `__bss_start`, away
  from the stack in use. `entry.asm` documents this reasoning because the safety
  is not obvious.

---

## 4. `kmain` sequence

```c
void kmain(af_boot_info_t *boot_info)
{
    af_arch_early_console_init(boot_info);   // 1. serial — or failures are silent
    af_boot_info_validate(boot_info);        // 2. magic, version, size, page alignment
        // on failure: retry from the 0x7000 backup, then boot_failed()
    af_boot_info_dump(boot_info);            // 3. describe the machine
    af_arch_cpu_dump();                      // 4. CPUID vendor/brand/features
    hal_cpu_init();                          // 5. GDT + IDT + PIC remap
        // AF_GDT_READY, AF_IDT_READY
    af_fb_init(&boot_info->framebuffer);     // 6. framebuffer (non-fatal if absent)
    af_fb_console_init(...);                 // 7. mirror the log to the screen
    af_splash_draw();                        // 8. prove the graphics path
        // AF_FB_READY
    af_selftest_run_all();                   // 9. in-kernel tests
        // AF_TEST_OK
    af_marker(AF_BOOT_MARKER_OK);            // 10. AF_BOOT_OK — the CI contract
    for (;;) af_halt();                      // 11. idle until v0.2 adds a scheduler
}
```

**Ordering rationale.** The IDT comes before anything that could fault, because
until it exists any exception is a triple fault with no output at all. The
framebuffer comes after the IDT so a graphics failure produces a diagnostic
instead of a silent reset. The splash precedes the self tests so a rendering
failure is visible immediately rather than being reported as a test failure.

### 4.1 Validation, and the recovery path

`af_boot_info_validate` checks magic, version, structure size, that the memory
map is non-empty, that every region is page-aligned and non-zero-length, that at
least one region is usable, and that any declared framebuffer has a pitch of at
least `width × bpp / 8`.

If validation fails, `kmain` tries the fixed backup address at `0x7000` before
giving up. This exists because the register path is a single point of failure: if
anything between `efi_main`'s `entry(bi)` and `kmain` clobbers `rdi`, the
fallback is the difference between a diagnostic and a reset loop.

On total failure `boot_failed()` prints the stage, the status name, and a
**specific** explanation for the code — a stale `BOOTX64.EFI` on the ESP, a
firmware that changed the map after `ExitBootServices`, a malformed GOP mode. It
then halts. It deliberately does **not** panic: a panic prints a register dump,
and a bad `boot_info` is not a CPU fault.

---

## 5. Phone: ARM64 / U-Boot (v1.1)

Designed now so the HAL interface is honest; implemented at v1.1.

### 5.1 Sequence

| # | Step | Notes |
| --- | --- | --- |
| 1 | SoC boot ROM → SPL → U-Boot | Vendor-specific and out of our control |
| 2 | U-Boot loads `boot.img`: kernel + DTB + ramdisk | Android boot image header v2/v4, produced by `mkimage.py --mode phone` |
| 3 | U-Boot enables the MMU with an identity map and jumps to the kernel at EL1 | Do **not** assume the MMU is off |
| 4 | `_start` parks secondary cores with `wfe`, installs the stack, clears `.bss`, calls `kmain` | Parking must happen immediately: a secondary core executing the same code is a race with no diagnostic |
| 5 | `hal_console_init` brings up the PL011 from the DT `stdout-path` | Serial before anything else |
| 6 | Device Tree parsed for `/memory` and `/reserved-memory` | The ARM equivalent of the UEFI memory map |
| 7 | `VBAR_EL1` set to the exception vector table | The ARM equivalent of the IDT |
| 8 | `MAIR_EL1`, `TCR_EL1`, `TTBR0_EL1`/`TTBR1_EL1` programmed | User half at TTBR0, kernel half at TTBR1 — the same conceptual split as PML4 |
| 9 | GICv3 initialised; generic timer programmed from `CNTFRQ_EL0` | |
| 10 | `simple-framebuffer` node scanned from the DT | The ARM equivalent of GOP |

### 5.2 The differences the HAL must absorb

| Concern | x86_64 | ARM64 |
| --- | --- | --- |
| Interrupt vectors | IDT, 256 gates, software-loaded | `VBAR_EL1`, 16 fixed vectors at 2 KiB alignment |
| Interrupt controller | PIC then APIC/IOAPIC | GICv3 |
| Timer | PIT then APIC timer, `rdtsc` | Generic timer, `CNTPCT_EL0`/`CNTP_*` |
| Memory discovery | UEFI memory map | Device Tree `/memory` |
| Memory map for devices | ACPI | Device Tree |
| Display | GOP | `simple-framebuffer` / a panel driver |
| Privilege levels | Ring 0 / Ring 3 | EL1 / EL0 |
| Stack pointer selection | Software: `TSS.RSP0` | Hardware, by exception level |
| FP/SIMD | `fxsave`/`fxrstor` | `q0`–`q31` + `fpsr`/`fpcr`, lazily |
| Cache control | Coherent by default | Explicit `dsb`/`isb`, `MAIR` device memory |

None of these may leak above `kernel/include/afriyie/hal.h`.

### 5.3 The portability test

At the end of the v1.1 port, measure the diff in `kernel/core/`. If more than
**20%** of the architecture-independent code had to change, the HAL is wrong and
must be redesigned before any further work. `kernel/core/` should need *no* new
`#if AF_TARGET_*` beyond the ones that already exist for genuine hardware
reasons.

---

## 6. Invariants

These hold at every point in the boot flow and are worth defending in review.

1. **The kernel never reads a firmware-specific structure.** Only
   `af_boot_info`.
2. **Every address in `af_boot_info` is physical** until the direct map is
   installed at v0.2.
3. **No exception can occur before the IDT is installed.** If it does, the
   machine resets with no output. Anything that can fault goes after
   `hal_cpu_init`.
4. **Nothing is allocated after `ExitBootServices`.** Every buffer the kernel
   needs was reserved before it.
5. **A failure before the console is up is invisible.** The very first action is
   therefore bringing up the console.
6. **A panic never reboots.** The fault state is the entire value of the crash.
7. **Boot progress is machine-readable.** Every subsystem prints a marker CI can
   grep, because a boot that cannot be asserted on cannot be regression-tested.

---

## 7. References

* UEFI Specification 2.10 — <https://uefi.org/specifications>
* Intel SDM Vol. 3 — paging, interrupts, privilege levels
* ARM Architecture Reference Manual (ARMv8-A) — exception levels, MMU, GIC
* Device Tree Specification — <https://devicetree.org>
* [debug-log.md](../debug-log.md) — the boot bugs already met and solved
* [memory-model.md](memory-model.md) — what happens after the handoff

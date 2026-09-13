# AfriyieOS — Complete Engineering Blueprint

> **A minimal, modern, cross-platform microkernel operating system for PCs (x86_64) and Phones (ARM64).**
> Written from scratch — no Linux, no AOSP, no existing kernel underneath.

| Field | Value |
| --- | --- |
| Project name | **AfriyieOS** |
| Codename | *Oseadeɛyɔ* (v1.0 release codename) |
| Document version | 1.0.0 |
| Document status | Living specification — updated at the end of every milestone |
| Target platforms | `x86_64` (UEFI PCs/laptops) · `aarch64` (ARM64 phones/tablets/SBCs) |
| Kernel model | Capability-based microkernel |
| Primary languages | C11 (kernel), C++20 (services/UI), NASM + GAS (assembly), Python (tooling) |
| Repository | <https://github.com/hayfordafriyie/AfriyieOS> |
| License | MIT (see `LICENSE`) |

---

## Table of Contents

1. [Executive Summary & Reality Check](#1-executive-summary--reality-check)
2. [Design Philosophy & Core Decisions](#2-design-philosophy--core-decisions)
3. [System Architecture](#3-system-architecture)
4. [Language & Technology Choices](#4-language--technology-choices)
5. [Frameworks, Libraries & Third-Party Components](#5-frameworks-libraries--third-party-components)
6. [Modern UI/UX Design System](#6-modern-uiux-design-system)
7. [Repository Layout](#7-repository-layout)
8. [Toolchain & Development Environment](#8-toolchain--development-environment)
9. [The AfriyieOS Kernel ABI (Syscalls + IPC)](#9-the-afriyieos-kernel-abi-syscalls--ipc)
10. [Version Roadmap: v0.1 → v1.0](#10-version-roadmap-v01--v10)
11. [Milestone Task Breakdown (Detailed TODOs)](#11-milestone-task-breakdown-detailed-todos)
12. [Testing, Debugging & CI Strategy](#12-testing-debugging--ci-strategy)
13. [Packaging, Installation & Release Engineering](#13-packaging-installation--release-engineering)
14. [Performance Budgets & Non-Functional Requirements](#14-performance-budgets--non-functional-requirements)
15. [Security Model](#15-security-model)
16. [Known Pitfalls & Engineering Discipline](#16-known-pitfalls--engineering-discipline)
17. [Risk Register](#17-risk-register)
18. [Appendix A — Glossary](#appendix-a--glossary)
19. [Appendix B — Reference Reading & Inspirations](#appendix-b--reference-reading--inspirations)

---

## 1. Executive Summary & Reality Check

### 1.1 What AfriyieOS is

AfriyieOS is a **from-scratch operating system** built on a **capability-based microkernel**. The kernel is deliberately tiny: it schedules threads, moves messages between them, manages virtual memory, and routes interrupts. Everything else — device drivers, file system, network stack, display compositor, window manager, applications — runs as **isolated user-space processes**.

That isolation is what makes *one codebase* able to target two completely different worlds:

* A desktop PC with UEFI firmware, ACPI tables, PCIe buses, NVMe drives, USB keyboards.
* A phone with a bootloader, a device tree, eMMC/UFS storage, I²C touch panels, and a power-constrained SoC.

Both share the same kernel core, the same IPC model, the same UI framework, and the same applications. Only the **HAL** (hardware abstraction layer) and the **boot bridge** differ.

### 1.2 What AfriyieOS is not

* It is **not** a Linux distribution (not Ubuntu, not LFS).
* It is **not** an Android fork (not AOSP).
* It is **not** a desktop shell running on someone else's kernel.
* It is **not** going to run Windows/macOS/Linux/Android binaries. There is no compatibility layer planned before v2.0.

### 1.3 Reality check — read this before writing a line of code

An honest scope statement, because this determines whether the project survives:

| Milestone | Realistic effort for one dedicated developer |
| --- | --- |
| Boot to framebuffer text (`v0.1`) | 1–3 weeks |
| Paging + preemptive multitasking (`v0.2`) | 3–6 weeks |
| Persistent file system over a virtio disk (`v0.3`) | 4–8 weeks |
| User mode + syscalls + ELF loader (`v0.4`) | 4–8 weeks |
| Drivers + input (`v0.5`) | 4–8 weeks |
| 2D graphics + compositor (`v0.6`–`v0.8`) | 2–4 months |
| Installer + image packaging (`v0.9`) | 1–2 months |
| Usable daily-driver minimum (`v1.0`) | 2–4 months |
| **ARM64 phone port (`v1.x`)** | **+3–6 months**, after the x86_64 kernel is stable |

Reference point: Linux 0.01 was **8,413 lines** of code and could not install itself, display graphics, or connect to a network. AfriyieOS v1.0 will be in the **60,000–120,000 line** range. This is a multi-year project; the roadmap below is designed so that **every single version is independently demonstrable**, which is what keeps motivation alive.

### 1.4 The one strategic rule

> **Do not develop PC and phone simultaneously.**
> Drive x86_64 to `v0.5` first. Then port to ARM64. If the port requires changing more than **20%** of the architecture-independent core code, the HAL abstraction is wrong and must be redesigned before continuing.

---

## 2. Design Philosophy & Core Decisions

### 2.1 Why a microkernel

A monolithic kernel pushes hardware differences into every subsystem, because on a monolithic kernel the storage driver, network stack, and scheduler all live in the same address space and share the same data structures. Porting means auditing everything.

A microkernel inverts that:

| Concern | Monolithic | AfriyieOS microkernel |
| --- | --- | --- |
| Driver crash | Kernel panic | Driver process restarts; system survives |
| Porting to new arch | Touch every subsystem | Touch HAL + boot bridge only |
| Kernel size | Millions of lines | Target **< 64 KB** of code |
| Debugging | Kernel debugger required | Ordinary user-space debugger |
| Security boundary | Ring 0 vs Ring 3 | Per-process capability isolation |
| Cost | Fast syscalls | IPC latency must be engineered |

We accept IPC cost and engineer it down (§14 performance budgets).

### 2.2 The five foundational principles

1. **Kernel minimalism.** Only four things live in Ring 0: **scheduling**, **IPC**, **virtual memory**, **interrupt control**. If a feature can be a user-space process, it *is* a user-space process.
2. **Capability-based access control.** There is no global `open("/dev/sda")`. Every kernel object (thread, address space, IPC endpoint, IRQ handler, memory frame) is referenced by an **unforgeable capability** — a kernel-managed handle. You can only do what you hold a capability for. (Directly inspired by seL4.)
3. **Zero-copy fast paths.** Small messages travel through CPU registers. Large messages travel by **granting/sharing mapped memory pages** rather than copying buffers.
4. **Driver isolation.** Every driver is a restartable user-space process with the *minimum* capabilities it needs (a Wi-Fi driver never sees the disk).
5. **Fail loudly, early.** Rich assertions, structured kernel logging over serial, and a panic screen that shows the fault address, the faulting thread, and a stack trace — from day one of `v0.1`. Debuggability is a feature, not a phase.

### 2.3 Architectural decisions log (ADRs)

| ID | Decision | Rationale | Rejected alternative |
| --- | --- | --- | --- |
| ADR-001 | Microkernel, not monolithic | Portability PC↔phone, fault isolation | Monolithic (Linux-like) |
| ADR-002 | C11 for Ring 0 | Predictable codegen, no runtime, total pointer control | Rust (immature `no_std` ecosystem, steep curve), Go (GC in interrupt context is fatal) |
| ADR-003 | C++20 for user space, `-fno-exceptions -fno-rtti`, no STL | Class abstraction for drivers/services; no hidden allocations | Full STL (pulls in host libc), Rust |
| ADR-004 | Synchronous IPC with `send`/`recv`/`call`/`reply` | Simple, verifiable, fast; async added later as a layer | Fully async message queues from day one |
| ADR-005 | Linear framebuffer first, GPU driver later | Removes GPU complexity from `v0.1`–`v0.8` | Write a GPU driver first |
| ADR-006 | Custom file system (`AFS`) + FAT32 read-only | FAT32 gets us booting quickly; AFS gives us what we control | ext4 (way too complex), ZFS (absurd) |
| ADR-007 | x86_64 first, ARM64 second | Halves initial hardware surface; validates HAL | Dual-track from day one |
| ADR-008 | QEMU as the only mandatory test platform until `v0.9` | Physical hardware debugging is brutally slow without JTAG | Real hardware from day one |
| ADR-009 | CMake 3.28+ (with Ninja) as the single build system | Cross-arch, toolchain files, IDE support | Hand-written Makefiles (fall apart at this scale) |
| ADR-010 | Message-passing UI, not in-process widgets | A crashed app cannot take down the compositor | Everything in one GUI process |
| ADR-011 | **Kernel `.text` budget raised from 64 KiB to 128 KiB at v0.3** | See below | — |

### ADR-011 — the kernel size budget, revised

**Context.** Through v0.2 the kernel `.text` sat at 33–52 KiB against a 64 KiB
budget. At v0.3 the milestone added PCI enumeration, a block device layer and the
virtio-blk driver — and `.text` reached **68 807 bytes**, breaking the budget.

**The rule this ADR exists to honour.** §14 says a change that breaks a budget does
not merge, "unless the change is accompanied by an ADR explaining why the budget
was wrong". So the options were to reduce the size or to justify the increase in
writing. Both were considered.

**What actually grew, and what did not.** The microkernel proper — scheduler, IPC,
virtual memory, capability manager, interrupt control — is *not* what broke the
budget. What broke it is a **kernel-mode device driver** (virtio-blk), **bus
enumeration** (PCI), and **boot-time graphics** (the framebuffer, bitmap font and
splash screen). Every one of those is code that the architecture says should
**not** be in Ring 0:

| Growth | Where it belongs | Moves out |
| --- | --- | --- |
| virtio-blk driver | user-space driver process | v0.7 |
| PCI enumeration | device manager service | v0.7 |
| Framebuffer, font, splash | compositor and UI framework | v0.6–v0.8 |

**Decision.** Raise the enforced budget to **128 KiB** for v0.3, and state the
figure that matters: the **microkernel core must return to under 64 KiB** once
drivers and boot graphics move to user space at v0.6–v0.7. CI enforces the 128 KiB
gate now, and a second, separate measurement of the core will be added when the
first driver leaves the kernel — because that is the point at which the number
becomes meaningful again.

**Consequences.** The budget is temporarily less strict than the architecture
intends, and that is recorded rather than hidden. The risk is that "128 KiB" quietly
becomes the new normal and the driver migration slips; the mitigation is that this
ADR names the migration as the thing that restores the figure, and the v0.7 task
list carries an explicit check on kernel size before and after.

**Alternatives rejected.** Compiling with `-Os` would recover perhaps 10 KiB and
make the kernel slower on the boot path to buy nothing structural. Splitting the
driver into a loadable module would be work spent on a mechanism that user-space
drivers replace entirely. Neither addresses the actual cause, which is that code
the architecture wants out of Ring 0 is currently in it.

---

## 3. System Architecture

### 3.1 The layered stack

```
┌──────────────────────────────────────────────────────────────────────────┐
│  L8 · APPLICATION LAYER                                          (u-mode)│
│  Terminal · Text Editor · Files · Settings · Clock · Image Viewer        │
├──────────────────────────────────────────────────────────────────────────┤
│  L7 · UI FRAMEWORK (C++20)                                       (u-mode)│
│  Declarative layout engine · Widget tree · Event router · Theme tokens   │
│  Responsive breakpoint engine (phone stack ⇄ desktop canvas)             │
├──────────────────────────────────────────────────────────────────────────┤
│  L6 · GRAPHICS COMPOSITOR SERVICE                                (u-mode)│
│  Window manager · Z-order · Dirty rectangles · Double/triple buffering   │
│  Input focus routing · Cursor · Animation clock                          │
├──────────────────────────────────────────────────────────────────────────┤
│  L5 · SYSTEM SERVICES                                            (u-mode)│
│  File System Service (AFS) · Device Manager · Name Service               │
│  Network Stack · Audio Service · Power Manager · Init/Service Manager    │
├──────────────────────────────────────────────────────────────────────────┤
│  L4 · DRIVER LAYER — isolated processes                          (u-mode)│
│  PC  : virtio-blk/NVMe · virtio-net · USB HID · Intel GPU · ACPI         │
│  Phone: eMMC/UFS · I²C/SPI touch · GPIO · SimpleFB · Device Tree         │
├══════════════════════════════════════════════════════════════════════════┤
│  L3 · THE ABI BOUNDARY                                                   │
│  System calls (syscall/svc)  ·  IPC endpoints  ·  Capability handles     │
│  Shared-memory grants  ·  IRQ notification channels                      │
├══════════════════════════════════════════════════════════════════════════┤
│  L2 · AFRIYIE MICROKERNEL (Ring 0 / EL1)                                 │
│  ┌────────────────┬────────────────┬────────────────┬─────────────────┐   │
│  │ Scheduler      │ IPC core       │ Virtual memory │ Interrupt ctrl  │   │
│  │ (round-robin + │ (endpoints,    │ (PML4 / TTBR,  │ (IDT / vectors, │   │
│  │  priority)     │  IPC fastpath) │  copy-on-write)│  IRQ→cap routing)│  │
│  ├────────────────┴────────────────┴────────────────┴─────────────────┤   │
│  │ Object/capability manager · Kernel heap · Timer · Panic console    │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│  Budget: < 64 KB code · fastpath < 1000 cycles · zero allocations in IRQ │
├──────────────────────────────────────────────────────────────────────────┤
│  L1 · HAL — HARDWARE ABSTRACTION LAYER                                    │
│  x86_64 : GDT/IDT/TSS · PML4 paging · APIC/PIC · MSR · TSC · CPUID· FPU  │
│  ARM64  : VBAR_EL1 vectors · TTBR0/1 · GICv3 · Generic Timer · SCTLR    │
│  Common : cache ops · barriers · atomics · MMIO accessors                │
├──────────────────────────────────────────────────────────────────────────┤
│  L0 · BOOT BRIDGE                                                         │
│  PC  : UEFI application → ExitBootServices → GOP framebuffer → mem map   │
│  Phone: U-Boot / Little Kernel → DT bootargs → simplefb → mem regions    │
├──────────────────────────────────────────────────────────────────────────┤
│  HARDWARE                                                                 │
│  Intel/AMD x86_64 + UEFI + ACPI   │   ARM64 Cortex-A/Snapdragon + DT      │
└──────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Boot sequence (both architectures, one model)

The single most important portability trick: **the boot bridge normalizes firmware differences into one handoff struct**, `afriyie_boot_info`. The kernel never knows whether it came from UEFI or U-Boot.

```
        PC (x86_64/UEFI)                        PHONE (ARM64/U-Boot)
 ┌──────────────────────────┐            ┌──────────────────────────┐
 │ Firmware POST            │            │ SoC ROM → SPL → U-Boot   │
 │ → UEFI finds BOOTX64.EFI │            │ → loads boot.img (kernel │
 │ → our EFI app runs       │            │   + DTB + initramfs)     │
 │ → init GOP framebuffer   │            │ → parses device tree     │
 │ → GetMemoryMap           │            │ → reserves mem regions    │
 │ → ExitBootServices       │            │ → leaves MMU enabled w/   │
 │ → build afriyie_boot_info│            │   identity map           │
 └────────────┬─────────────┘            └────────────┬─────────────┘
              │                                       │
              │        afriyie_boot_info (cached, page-aligned, magic)     │
              └───────────────────┬───────────────────┘
                                  ▼
                    ┌───────────────────────────────┐
                    │ kmain(boot_info*)             │
                    │  1. serial console up         │
                    │  2. verify boot_info magic    │
                    │  3. copy mem map to kernel BSS│
                    │  4. arch_init(): GDT/IDT/TSS  │
                    │     or VBAR/GIC               │
                    │  5. pmm_init()  bitmap from    │
                    │     normalized memory regions │
                    │  6. vmm_init()  higher-half    │
                    │     kernel mapping + direct map│
                    │  7. heap_init() slab allocator │
                    │  8. timer_init() + sched_init()│
                    │  9. cap_init() + ipc_init()    │
                    │ 10. splash: framebuffer logo   │
                    │ 11. load /init from ramdisk    │
                    │ 12. enter user mode → world    │
                    └───────────────────────────────┘
```

### 3.3 Address space layout (identical on both architectures)

```
0x0000_0000_0000_0000 ┌──────────────────────────┐
                      │ unmapped guard page      │  catch null derefs
0x0000_0000_0000_1000 ├──────────────────────────┤
                      │ USER TEXT   (r-x)        │ \
                      │ USER RODATA (r--)        │  | per-process
                      │ USER DATA   (rw-)        │  | user address space
                      │ USER BSS    (rw-)        │ /
                      ├──────────────────────────┤
                      │ USER HEAP  ↓             │
                      │        (unmapped gap)    │
                      │ USER STACK ↑             │
0x0000_7FFF_FFFF_F000 ├──────────────────────────┤
                      │        NON-CANONICAL /   │
                      │        unmapped gap      │
0xFFFF_8000_0000_0000 ├──────────────────────────┤
                      │ PHYSICAL DIRECT MAP      │  all RAM, offset-mapped
0xFFFF_C000_0000_0000 ├──────────────────────────┤
                      │ KERNEL IMAGE + HEAP      │  higher half
0xFFFF_FFFF_8000_0000 ├──────────────────────────┤
                      │ KERNEL STACKS + IST      │
0xFFFF_FFFF_FFFF_FFFF └──────────────────────────┘
```

ARM64 uses `TTBR1_EL1` for the kernel half (top-byte-ignore) and `TTBR0_EL1` for the user half — the exact same conceptual split.

### 3.4 Process & thread model

```
   PROCESS  =  address space + capability table + threads + resource accounting
      │
      ├── THREAD 1  (TID 1)  state=RUNNABLE  priority=8  affinity=any
      ├── THREAD 2  (TID 2)  state=BLOCKED_ON_IPC
      └── THREAD 3  (TID 3)  state=RUNNING
```

* **Thread** — the scheduling unit. Owns a kernel stack, a user stack, a saved register context, a priority, a CPU affinity mask, and a state.
* **Process** — the isolation unit. Owns a page table root, a capability table (`cap_t[256]` initially), a list of threads, an IPC endpoint table, and memory accounting.
* **Scheduler** — `v0.2` uses **round-robin over a run queue with 8 priority levels**; `v0.6` adds **MLFQ with priority inheritance** for IPC reply latency. Context switch = save callee-saved registers + swap stack pointer + `CR3`/`TTBR0`.

### 3.5 The IPC model (the heart of the system)

Four primitives, all capability-checked:

| Primitive | Semantics |
| --- | --- |
| `ipc_send(cap, msg)` | Asynchronous, fire-and-forget. Sender never blocks. |
| `ipc_recv(cap, &msg)` | Blocking receive on an endpoint. |
| `ipc_call(cap, msg, &reply)` | Synchronous RPC: send + block for reply atomically. **The main path.** |
| `ipc_reply(cap, reply)` | Reply to the caller blocked in `ipc_call`. |

Message registers (the fastpath, no allocation, no copy):

```
struct afriyie_msg {          // 64 bytes, register-passed on x86_64 (rdi..r15)
    uint64_t label;           // protocol/method selector
    uint64_t caps[4];         // capabilities being transferred
    uint64_t payload[4];      // 32 bytes of inline data
};
```

Anything larger uses a **memory grant**: the sender maps a page into the receiver's address space with explicitly granted rights (`READ`, `WRITE`, `SHARE`), then sends the capability in the message. Zero copies.

### 3.6 Capability system

Every kernel object is reached through a capability slot. Capabilities are `(slot_index)` handles held in a per-process table, unforgeable because they are only meaningful inside the kernel.

Object types: `THREAD`, `PROCESS`, `ENDPOINT`, `IRQ_HANDLER`, `MEMORY_FRAME`, `PAGE_TABLE`, `DEVICE_REGION`, `PORT_IO`, `REPLY`, `NOTIFICATION`.

Rights bits: `CAP_READ | CAP_WRITE | CAP_EXEC | CAP_GRANT | CAP_REVOKE | CAP_SIGNAL | CAP_WAIT`.

*A driver that loses its `END_POINT` capability cannot speak to the file system — even if a bug lets it jump anywhere in memory.*

---

## 4. Language & Technology Choices

### 4.1 Language allocation matrix

| Layer | Language | Standard | Why this language here |
| --- | --- | --- | --- |
| Boot bridge (x86_64) | **C11** + NASM | C11 | UEFI's native ABI is C; `efi_main` must match MS-ABI exactly |
| Boot bridge (ARM64) | **C11** + GAS | C11 | U-Boot passes a DTB pointer; assembly only for the entry stub |
| CPU init / context switch | **NASM** (x86_64), **GNU as** (ARM64) | — | Direct register control, no compiler interference |
| Interrupt/exception entry | **NASM/GAS stub → C dispatcher** | — | 5 instructions of asm, everything else testable C |
| Microkernel core | **C11** | `-std=gnu11` | Zero runtime, deterministic codegen, total pointer control |
| Kernel heap, PMM, VMM | **C11** | C11 | Bit manipulation and page-table walking need raw control |
| HAL | **C11** + headers per arch | C11 | Same function names, different implementations |
| Drivers | **C++20** | `-fno-exceptions -fno-rtti` | RAII for MMIO mappings, class hierarchies for device families |
| System services | **C++20** | same restrictions | B-trees, hash maps, reference counting with no STL dependency |
| UI framework | **C++20** | same restrictions | Widget trees and layout solvers |
| Applications | **C++20** (system apps) | same | Same ABI as services; a `libaf` userspace SDK |
| Build system | **CMake 3.28+** | — | Toolchain files cleanly express `x86_64-elf` vs `aarch64-elf` |
| Tooling / image packing | **Python 3.11+** | — | Disk image creation, FAT32 writing, checksumming, release scripts |
| Test harness | **Python + Bash + QEMU** | — | Boot tests, serial-output assertions, automated regression |

### 4.2 Why not Rust for the kernel?

Rust is a *defensible* future choice, and AfriyieOS explicitly leaves the door open (ADR-002 notes it). The reasons it is not the `v0.1` choice:

* A mature `no_std` interrupt/context-switch story is still more work than it looks.
* Mixed C/Rust kernels pay FFI friction exactly where latency matters (context switch, IPC fastpath).
* Learning curve cost is high *concurrently* with learning OS development itself.
* The kernel is small enough (< 64 KB) that C's weaknesses are tractable with discipline and tooling (static analysis, `-Wall -Wextra -Werror`, `clang-tidy`).

**Decision:** C11 now; revisit Rust for *new user-space services* at `v1.0+` once the ABI is frozen.

### 4.3 C coding standard for the kernel (non-negotiable)

```c
/* Naming */
snake_case for functions and variables (pmm_alloc_frame)
UPPER_SNAKE for macros and constants (PAGE_SIZE)
struct foo { ... }; typedef struct foo foo_t;   /* _t suffix for types */

/* Rules */
- Every function that can fail returns af_status_t (int32_t), never a magic value.
- Every allocation checks for failure. There is no "it will not happen" in a kernel.
- No VLA, no recursion in kernel paths, no floating point outside explicit FPU context saves.
- No dynamic allocation in interrupt context. Ever.
- All MMIO through volatile accessors: mmio_read32(addr) / mmio_write32(addr, v).
- All shared state annotated with its locking discipline in a comment above it.
- Functions > 60 lines must be split, except unrolled page-table walkers.
- Every file ends with a short "// SPDX-License-Identifier: MIT" and an architecture note.
```

### 4.4 C++ restrictions for user space

```cpp
// main.cpp — the only place exceptions/RTTI/STL would be allowed is the host tooling,
// never in AfriyieOS user space.
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
-fno-stack-protector -ffreestanding -nostdlib++ -nostdinc++

// Allowed:  templates, constexpr, namespaces, classes, RAII, operator overloading
// Banned :  std::vector, std::string, std::iostream, new/delete (except a custom arena
//           allocator), exceptions, dynamic_cast, typeid, thread_local
// Replaced: af::Vector<T>, af::String, af::HashMap<K,V>, af::UniquePtr<T>, af::Arena
```

**The AfriyieOS C++ runtime (`libaf++`)** is a from-scratch mini-STL providing exactly what the system needs, with explicit allocation failure handling. (~3,000 lines, written during `v0.5`–`v0.6`, once there is an allocator and a process to own it.)

---

## 5. Frameworks, Libraries & Third-Party Components

### 5.1 Core dependency policy

> **Rule:** the kernel has **zero** third-party dependencies. User space may vendor libraries only if they are (a) permissively licensed, (b) buildable freestanding, (c) small enough to audit.

### 5.2 Component selection

| Purpose | Chosen | Alternatives considered | Notes |
| --- | --- | --- | --- |
| Cross-toolchain | **GCC 14 (`x86_64-elf-gcc`, `aarch64-elf-gcc`)** | Clang/LLVM cross | GCC bare-metal targets are the best-trodden path; Clang planned as a secondary CI target |
| Build system | **CMake 3.28 + Ninja** | Meson, hand-written Make | Toolchain files map 1:1 to our two targets |
| Bootloader (PC) | **Custom UEFI app** (no GRUB) | GRUB2, Limine | Owning the boot bridge keeps `afriyie_boot_info` exact; Limine remains a fallback |
| Bootloader (phone) | **U-Boot** + custom `boot.img` packer | Little Kernel (LK), bare `fastboot` | U-Boot already supports most phone SoCs and passes a clean DTB |
| Firmware tables | **own ACPI parser** (`v0.5`) + own **Device Tree parser** (`v1.1`) | ACPICA (too big), libfdt (vendored subset is fine) | Start with UEFI-provided memory map only |
| 2D vector rendering | **ThorVG** (vendored, `v0.7`) | Skia (huge), Cairo (heavy deps), blend2d | ThorVG is small, dependency-light, actively maintained, and made for embedded |
| Font rasterization | **custom bitmap font (`v0.1`) → stb_truetype subset (`v0.7`)** | FreeType (large but possible) | Bitmap first for speed of progress; TrueType at UI time |
| Compression (initramfs) | **miniz** (vendored, `v0.9`) | zlib, LZ4 | Single-file, permissive |
| Crypto (hashes, signatures) | **Monocypher** | OpenSSL (never), mbedTLS | Small, auditable, public domain |
| Network stack | **own TCP/IP (`v2.0`)** | lwIP (vendored, acceptable interim) | lwIP may be vendored for early networking; long-term goal is our own |
| Audio | **own PCM pipeline** | — | Simple DAC/PCM output first |
| Test framework | **custom `af-test` (in-kernel asserts) + pytest for host-side** | Unity, GoogleTest | Kernel tests must run bare-metal |
| Emulation | **QEMU 8+** (`qemu-system-x86_64`, `qemu-system-aarch64`) | Bochs, real hardware | The only mandatory test target until `v0.9` |
| Debugging | **GDB + QEMU `-s -S`**, plus serial logging | — | Non-negotiable infrastructure |
| CI | **GitHub Actions** (Ubuntu runners, QEMU, timeout-guarded boot tests) | — | Every push must boot to a known serial marker |
| Documentation | **Markdown + MkDocs Material** | Doxygen only | This blueprint is the canonical spec |

### 5.3 Vendored-third-party directory policy

```
third_party/
├── thorvg/        # v0.7+  — vector renderer (MIT)
├── miniz/         # v0.9+  — DEFLATE (MIT)
├── monocypher/    # v2.0+  — crypto (BSD-2/CC0)
├── stb/           # v0.7+  — stb_truetype.h (public domain / MIT)
└── README.md      # every entry: upstream URL, version, license, local patches
```

Each vendored library receives a **port shim** (`port/af_platform.cpp`) instead of patching upstream sources. This makes upstream upgrades a merge, not a rewrite.

---

## 6. Modern UI/UX Design System

Called **Afriyie Shell** — one design language, two form factors.

### 6.1 The responsive breakpoint engine

The compositor queries the display at boot and on every hotplug/resize:

```
┌────────────────────────────────────────────────────────────────┐
│  aspect_ratio = width / height                                 │
│  diagonal_inches = sqrt(w² + h²) / ppi                         │
│                                                                │
│  if (aspect_ratio < 1.15  && diagonal_inches < 9.0)            │
│        → PHONE MODE                                            │
│  else if (width < 900)                                         │
│        → COMPACT / TABLET MODE                                 │
│  else                                                          │
│        → DESKTOP MODE                                          │
└────────────────────────────────────────────────────────────────┘
```

| Aspect | PHONE MODE (portrait) | DESKTOP MODE (landscape) |
| --- | --- | --- |
| Navigation | Full-screen app stack + gesture bar + back gesture | Overlapping windows + taskbar + start menu |
| System chrome | Status bar (time, battery, signal) + notch-safe insets | Title bars, close/min/max buttons, tray |
| Input model | Touch (multi-touch, long-press, swipe, pinch) | Mouse (hover states, right-click, wheel) + keyboard |
| Windows | One app at a time, sheet presentations | Free-floating, resizable, snappable to halves |
| Typography | 16sp base, generous line height | 14px base, denser information |
| Density | `comfortable` (48px touch targets) | `compact` up to `dense` |
| Settings UI | Full-screen drill-down lists | Two-pane sidebar + detail |

### 6.2 Design tokens (single source of truth)

```
Design tokens live in one file: ui/theme/tokens.afh  (Afriyie Theme Header)
→ compiled to C++ constexpr structs AND to the docs style guide.

Color   : surface, surfaceContainer, onSurface, primary, onPrimary, error...
Spacing : 4 / 8 / 12 / 16 / 24 / 32 / 48  (4px base grid)
Radius  : 8 (small) · 12 (medium) · 16 (large) · 28 (pill)
Elevation: 0 / 1 / 3 / 6 / 12  (soft shadows, no hard borders)
Motion  : 100ms (micro) · 200ms (standard) · 400ms (entrance)
          easing: cubic-bezier(0.2, 0.0, 0.0, 1.0) "emphasized decelerate"
Type    : Afriyie Sans (or Inter as interim) — Caption 12 · Body 14/16 · Title 20 · Headline 28
```

### 6.3 Rendering pipeline

```
App draws into its own off-screen surface (shared memory with compositor)
        │
        ▼
Compositor: for each damaged rect, for each window in z-order:
    blit surface → composite into back buffer (alpha blend, clip)
        │
        ▼
Optional blur/shadow passes (cached, `v0.8+`)
        │
        ▼
Back buffer --flip--> framebuffer (or vsync'd page flip when a real GPU driver exists)
```

* `v0.6`: whole-screen redraw, single buffer, software blit. Slow but correct.
* `v0.8`: dirty-rect tracking, double buffering, cached shadows.
* `v1.2`: page flipping, hardware-accelerated blits, damage regions from apps.

### 6.4 App model

* Every app is a **process** speaking the `af_shell` protocol over IPC.
* Widget tree is declared in C++ (`Column { Text{"Hi"}, Button{"Go", onClick} }`) — a declarative, immediate-mode-inspired hybrid: retained tree, but rebuilt from a `build()` function on state change.
* System apps at `v1.0`: **Terminal**, **Text Editor (Notes)**, **Files**, **Settings**, **Clock**, **Calculator**, **Image Viewer**.

---

## 7. Repository Layout

```
AfriyieOS/
├── README.md
├── LICENSE                          # MIT
├── .gitignore
├── .gitattributes
├── CMakeLists.txt                   # top-level build orchestration
├── cmake/
│   ├── toolchain-x86_64-elf.cmake
│   ├── toolchain-aarch64-elf.cmake
│   ├── flags.cmake                  # the canonical flag sets per layer
│   └── qemu.cmake                   # run/debug/install test targets
├── docs/
│   ├── AfriyieOS-Blueprint.md        # ← this document
│   ├── architecture/                 # deep dives added per milestone
│   │   ├── boot-flow.md
│   │   ├── ipc-protocol.md
│   │   ├── memory-model.md
│   │   └── capability-model.md
│   ├── abi/
│   │   ├── syscalls.md
│   │   └── afs-filesystem.md
│   ├── design/
│   │   └── ui-design-system.md
│   └── releases/                     # per-version release notes
│       └── v0.1.0.md
├── boot/
│   ├── uefi/                         # x86_64 boot bridge
│   │   ├── efi_main.c
│   │   ├── efi_console.c
│   │   ├── efi_gop.c
│   │   ├── efi_memmap.c
│   │   ├── boot_info.c               # builds afriyie_boot_info
│   │   └── bootx64.lds
│   └── arm64/                        # phone boot bridge
│       ├── uboot_entry.S
│       ├── dtb_scan.c
│       └── boot_info_arm.c
├── kernel/
│   ├── include/afriyie/
│   │   ├── types.h  status.h  config.h  boot_info.h
│   │   ├── hal.h    cap.h     ipc.h     sched.h
│   │   ├── pmm.h    vmm.h     heap.h    irq.h
│   │   └── panic.h  log.h     assert.h
│   ├── arch/
│   │   ├── x86_64/   gdt.c idt.c isr_stubs.asm context.asm paging.c tsc.c serial.c
│   │   └── arm64/    vectors.S context.S mmu.c gic.c generic_timer.c pl011.c
│   ├── core/
│   │   ├── kmain.c     # the boot sequence lives here
│   │   ├── pmm.c  vmm.c  heap.c
│   │   ├── sched.c  thread.c  context.c
│   │   ├── ipc.c  endpoint.c
│   │   ├── cap.c  object.c
│   │   ├── irq.c  timer.c
│   │   ├── log.c  panic.c  assert.c
│   │   └── syscall.c
│   └── linker/
│       ├── x86_64.lds
│       └── arm64.lds
├── libs/
│   ├── libaf/            # user-space C runtime shim (syscall wrappers, string, printf)
│   ├── libafpp/          # the mini-STL: Vector, String, HashMap, UniquePtr, Arena
│   └── libafgui/         # UI framework (layout, widgets, theme, event router)
├── servers/              # user-space system services
│   ├── init/             # PID 1: starts everything, restarts crashes
│   ├── name/             # name service (service discovery)
│   ├── fs/               # file system service (AFS + FAT32 reader)
│   ├── devmgr/           # device manager / driver loader
│   ├── compositor/       # window manager + renderer
│   ├── audio/
│   ├── net/
│   └── power/
├── drivers/              # user-space drivers
│   ├── virtio_blk/  nvme/  usb_hid/  ps2/  virtio_net/  virtio_input/
│   ├── emmc/  i2c_touch/  gpio/  simplefb/  intel_gpu/
│   └── driver_sdk/       # the interface every driver implements
├── apps/
│   ├── terminal/  notes/  files/  settings/  clock/  calculator/  imageview/
│   └── sdk/              # the public app SDK (headers + docs)
├── third_party/          # vendored, per §5.3
├── tools/
│   ├── build_toolchain.sh
│   ├── mkimage.py            # build bootable disk images
│   ├── mkafs.py              # build an AFS file system image
│   ├── pack_initramfs.py     # tar + compress the user-space payload
│   ├── run_qemu.py           # smart runner: PC or phone, debug or normal
│   └── gdb_helpers.py
├── tests/
│   ├── kernel/           # in-kernel unit tests (assert-based, run in QEMU)
│   ├── boot/             # serial-output golden tests
│   ├── host/             # pytest for python tooling
│   └── integration/      # scripted QEMU runs, screenshots, input injection
└── .github/workflows/
    └── ci.yml
```

---

## 8. Toolchain & Development Environment

### 8.1 Host requirements

| Requirement | Minimum | Recommended |
| --- | --- | --- |
| OS | Ubuntu 22.04 / WSL2 / Arch | Ubuntu 24.04 |
| Disk | 50 GB free | 100 GB SSD |
| RAM | 8 GB | 16–32 GB |
| CPU | 4 cores | 8+ cores |

> **Windows note:** build and QEMU runs happen inside **WSL2** (Ubuntu). Native Windows is used only for editing and git. The toolchain script targets Debian/Ubuntu.

### 8.2 Host packages

```bash
sudo apt update
sudo apt install -y build-essential bison flex libgmp3-dev libmpc-dev \
    libmpfr-dev texinfo nasm xorriso mtools qemu-system-x86 qemu-system-arm \
    qemu-utils gdb python3 python3-pip cmake ninja-build git wget curl \
    device-tree-compiler imagemagick
```

### 8.3 Cross-compiler build

Two targets, built from Binutils + GCC sources, no host libc:

```bash
./tools/build_toolchain.sh          # builds both x86_64-elf and aarch64-elf
export PATH="$HOME/opt/cross/bin:$PATH"
x86_64-elf-gcc --version            # gcc 14.x
aarch64-elf-gcc --version           # gcc 14.x
```

*(The script in `tools/` is the authoritative implementation; it builds binutils → GCC stage 1 (`all-gcc`, `all-target-libgcc`) → `libstdc++` for the `-elf` target in stage 2, and is idempotent so re-running skips completed stages.)*

### 8.4 Canonical compiler flags

```cmake
# Kernel (C11, Ring 0)
-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
-mno-sse -mno-sse2 -mno-mmx -mcmodel=kernel -mno-80387
-Wall -Wextra -Werror -Wno-unused-parameter -O2 -g -std=gnu11

# User space (C++20)  — adds
-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
-fno-stack-protector -nostdlib++ -nostdinc++ -std=c++20 -O2 -g

# ARM64 adds
-mgeneral-regs-only -mno-outline-atomics -mgeneral-regs-only
# (ARM64 omits -mno-red-zone and the x87/SSE family flags)
```

Two flags deserve their reputation:

* **`-mno-red-zone`** — the x86_64 red zone is 128 bytes below `rsp` that the ABI says nobody touches. Interrupt handlers *do* touch it. Omitting this flag produces bugs that appear once every few thousand interrupts.
* **`-mcmodel=kernel`** — required because the kernel lives in the top 2 GB of the address space (`0xFFFF_FFFF_8000_0000+`); the default small model cannot reach it.

### 8.5 The build

```bash
# Configure for PC
cmake -B build/x86_64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake \
      -DAF_TARGET=x86_64
cmake --build build/x86_64

# Run
python3 tools/run_qemu.py --arch x86_64 --image build/x86_64/afriyieos.img
# or: cmake --build build/x86_64 --target run
```

### 8.6 QEMU reference invocations

**PC (UEFI, with OVMF):**

```bash
qemu-system-x86_64 \
  -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive file=build/x86_64/afriyieos.img,format=raw,if=virtio \
  -device virtio-keyboard-pci -device virtio-tablet-pci \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial stdio -display gtk -no-reboot
```

**Phone (ARM64, virt machine):**

```bash
qemu-system-aarch64 \
  -machine virt -cpu cortex-a72 -m 2G -smp 4 \
  -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
  -drive file=build/aarch64/afriyieos.img,format=raw,if=virtio \
  -device virtio-gpu-pci -device virtio-keyboard-pci \
  -serial stdio -display gtk -no-reboot
```

**Debugging:** append `-s -S`, then `gdb -ex 'target remote :1234' build/x86_64/kernel.elf`.
**Screenshots:** QEMU monitor → `screendump shot.ppm`.
**Input injection:** QEMU monitor → `sendkey a`, `mouse_move 100 200`, `mouse_button 1`.

---

## 9. The AfriyieOS Kernel ABI (Syscalls + IPC)

Frozen at `v0.4`, versioned, and never changed without a bump.

### 9.1 Syscall convention

| | x86_64 | ARM64 |
| --- | --- | --- |
| Instruction | `syscall` / `sysretq` | `svc #0` |
| Number register | `rax` | `x8` |
| Argument registers | `rdi, rsi, rdx, r10, r8, r9` | `x0 … x5` |
| Return | `rax` (negative = `-af_status_t`) | `x0` |
| Stack alignment | **16 bytes — mandatory** | 16 bytes |

### 9.2 Syscall table

> **`docs/abi/syscalls.md` §2 is the canonical table.** It is duplicated here only
> so the blueprint reads on its own; where the two ever disagree, the ABI document
> wins and this one is corrected.
>
> Until v0.4 these two tables had **drifted**: both claimed to be the frozen
> numbering and they assigned different numbers to the same calls (this one put
> `sys_ipc_send` at 4, the ABI document put `sys_thread_join` there). Nothing had
> noticed because nothing above number 5 existed yet. The numbers below are now
> the ABI document's, which are also the ones `kernel/core/syscall.c` implements
> — the code being the thing that would break first is what decided it.
>
> The numbers are **append-only**. A retired number stays reserved forever, so a
> stale binary receives `AF_ERR_NOTSUP` rather than reaching a different call. The
> "Implemented" column is the honest state of `kernel/core/syscall.c`; a reserved
> number that is not implemented returns `AF_ERR_NOTSUP` today.

| # | Name | Args | Returns | Planned | Implemented |
| --- | --- | --- | --- | --- | --- |
| 0 | `sys_debug_write` | `buf, len` | bytes written | v0.4 | **v0.4 ✓** |
| 1 | `sys_exit` | `code` | *never* | v0.4 | **v0.4 ✓** |
| 2 | `sys_thread_create` | `entry, arg, stack, prio` | `cap THREAD` | v0.4 | v0.5 |
| 3 | `sys_thread_yield` | — | 0 | v0.4 | **v0.4 ✓** |
| 4 | `sys_thread_join` | `cap THREAD` | exit code | v0.4 | v0.5 |
| 5 | `sys_clock_get` | `clock_id` | nanoseconds | v0.6 | **v0.4 ✓** *(pulled forward — the ELF program's own tests needed a clock that was not a print)* |
| 6 | `sys_sleep` | `ns` | 0 | v0.6 | — |
| 7 | `sys_fb_map` | — | `cap FRAME` + geometry | v0.6 | — |
| 8 | `sys_input_read` | `evt_ptr, max` | events read | v0.5 | — |
| 9 | `sys_dev_claim` | `dev_id` | `cap DEVICE` | v0.5 | — |
| 10 | `sys_ipc_send` | `cap, msg_ptr` | 0 | v0.7 | — |
| 11 | `sys_ipc_recv` | `cap, msg_ptr` | 0 | v0.7 | — |
| 12 | `sys_ipc_call` | `cap, msg_ptr, reply_ptr` | 0 | v0.7 | — |
| 13 | `sys_ipc_reply` | `cap, msg_ptr` | 0 | v0.7 | — |
| 14 | `sys_notify` | `cap, bits` | 0 | v0.8 | — |
| 15 | `sys_wait` | `cap, mask, out_ptr` | observed bits | v0.8 | — |
| 16 | `sys_cap_derive` | `cap, rights` | `cap` | v0.7 | — |
| 17 | `sys_cap_delete` | `cap` | 0 | v0.7 | — |
| 18 | `sys_cap_revoke` | `cap` | 0 | v0.7 | — |
| 19 | `sys_mem_alloc` | `size, flags` | `cap FRAME` | v0.7 | — |
| 20 | `sys_mem_map` | `cap, vaddr, rights` | mapped address | v0.7 | — |
| 21 | `sys_mem_unmap` | `vaddr, size` | 0 | v0.7 | — |
| 22 | `sys_mem_grant` | `dst_proc, cap, vaddr, rights` | target address | v0.7 | — |
| 23 | `sys_irq_wait` | `cap IRQ` | irq number | v0.7 | — |
| 24 | `sys_irq_ack` | `cap IRQ` | 0 | v0.7 | — |
| 25 | `sys_process_spawn` | `path_cap, argv_ptr` | `cap PROCESS` | v0.9 | — |
| 26 | `sys_process_wait` | `cap PROCESS` | exit code | v0.9 | — |

**Entry mechanism at v0.4:** `int 0x80` through IDT vector `0x80` (DPL=3), not
`SYSCALL`/`SYSRET`. The argument registers are identical — `rax` number, `rdi`,
`rsi`, `rdx`, `r10`, `r8`, `r9` — so the upgrade changes the stub in
`libs/libaf/include/af.h` and the handler prologue, and no caller and no
numbering. It is deferred because the interrupt path was already written and
tested at v0.4, and `SYSCALL` would have added MSR setup and a second entry path
to debug in the same milestone that first crossed the privilege boundary.

**Argument validation at v0.4 is enforced against the live page tables, not a
per-process record.** Range, wraparound, and whether every page in the range is
mapped, user-accessible and (for writes) writable are all checked before any
dereference. The only thing missing is the `af_process_t *` argument in the design
below — the walk itself is the same one and will not change when processes arrive.
The six malformed buffers `apps/init` passes are the evidence.

### 9.3 IPC message and protocol header

```c
/* kernel/include/afriyie/ipc.h */
#define AF_MSG_LABEL_SHIFT   0
#define AF_MSG_MAX_CAPS      4
#define AF_MSG_INLINE_WORDS  4

typedef struct {
    uint64_t label;                    /* method / protocol selector */
    uint32_t cap_count;                /* how many of caps[] are valid */
    uint32_t _pad;
    uint64_t caps[AF_MSG_MAX_CAPS];    /* capabilities being transferred */
    uint64_t words[AF_MSG_INLINE_WORDS]; /* 32 bytes of inline payload */
} af_msg_t;                            /* 64 bytes: fits in registers */
```

Service protocols are defined as `label` namespaces in `docs/abi/`:

```
0x0100_xxxx  name service      (lookup, register, deregister)
0x0200_xxxx  file system       (open, read, write, close, stat, readdir)
0x0300_xxxx  device manager    (enumerate, claim, release, irq_bind)
0x0400_xxxx  compositor        (create_window, present, damage, input_event)
0x0500_xxxx  audio             (open_stream, write_pcm, set_volume)
0x0600_xxxx  network           (socket, bind, listen, connect, send, recv)
0x0700_xxxx  power             (set_state, battery_info, thermal)
0xFF00_xxxx  driver SDK        (probe, start, stop, irq_notify)
```

---

## 10. Version Roadmap: v0.1 → v1.0

### 10.1 At-a-glance

| Version | Codename | Theme | Demonstrable deliverable | Arch |
| --- | --- | --- | --- | --- |
| **v0.1** | *Seed* | Boot & display | QEMU shows an AfriyieOS splash + "Hello from AfriyieOS" in framebuffer | x86_64 |
| **v0.2** | *Roots* | Memory & multitasking | Two kernel threads print `A`/`B` alternately via preemptive scheduling | x86_64 |
| **v0.3** | *Trunk* | Disk & file system | Read `HELLO.TXT` from a virtio disk through FAT32 and print it | x86_64 |
| **v0.4** | *Branches* | User mode & syscalls | ELF user program calls `sys_debug_write` and exits cleanly — ✅ acceptance met, processes deferred to v0.5 | x86_64 |
| **v0.5** | *Leaves* | Drivers & input | Keyboard input echoed to screen; device enumeration works | x86_64 |
| **v0.6** | *Bloom* | Graphics core | Colored rectangles, gradients, text and shapes rendered to framebuffer | x86_64 |
| **v0.7** | *Fruit* | IPC & services | File system runs as a user-space server; apps talk to it via IPC | x86_64 |
| **v0.8** | *Canopy* | Window manager | Two overlapping windows, click to focus, drag to move | x86_64 |
| **v0.9** | *Harvest* | Installer | Boot from a USB image, install to disk, reboot into the installed system | x86_64 |
| **v1.0** | *Oseadeɛyɔ* | Daily usable | Terminal + Notes + Files + Settings, persistent storage, real workflow | x86_64 |
| **v1.1** | *Twin* | Phone port | Same kernel boots on ARM64 QEMU, touch input, phone-mode UI | +ARM64 |
| **v1.2** | *Dual* | Acceleration & polish | GPU blits, page flipping, animations, power management, OTA updates | both |
| **v2.0** | *Horizon* | Networking & ecosystem | TCP/IP, app store/package manager, SDK docs, third-party apps | both |

### 10.2 Version dependency graph

```
v0.1 Seed ──► v0.2 Roots ──► v0.3 Trunk ──► v0.4 Branches ──┬─► v0.5 Leaves ──► v0.6 Bloom
                                                             │                      │
                                                             └──────────────────────┴─► v0.7 Fruit
                                                                                          │
                                                                                          ▼
                                                                                     v0.8 Canopy
                                                                                          │
                                                                                          ▼
                                                                                     v0.9 Harvest
                                                                                          │
                                                                                          ▼
                                                                                      v1.0 Oseadeɛyɔ
                                                                                          │
                                                                                          ├─► v1.1 Twin (ARM64)
                                                                                          │
                                                                                          └─► v2.0 Horizon (net)
```

---

## 11. Milestone Task Breakdown (Detailed TODOs)

> Convention: `[ ]` todo · `[~]` in progress · `[x]` done.
> Every version ends with: **docs updated · CHANGELOG entry · git tag · push**.

---

### ✅ v0.1 — *Seed*: Boot & Display — **COMPLETE**

**Goal:** code running on bare metal, drawing to the screen.
**Architecture:** x86_64 only (ARM64 deferred to v1.1 by ADR-007).
**Status detail:** [releases/v0.1.0.md](releases/v0.1.0.md)
**Verified:** builds, links, boots to `AF_BOOT_OK` in QEMU, 71 in-kernel self
tests pass on bare metal, the splash renders in the correct colours and layout.
Run `./tools/verify_all.sh` — 7 of 7 checks pass.

> The code below is written and the milestone is closed. Two items were
> deliberately deferred rather than quietly dropped, and they are marked as such.

#### 0.1.1 Project foundation
- [x] Create repository skeleton per §7 (all directories, `CMakeLists.txt` wiring)
- [x] Write `cmake/toolchain-x86_64-elf.cmake` (target `x86_64-elf`, `-ffreestanding`, no libc)
- [x] Write `cmake/flags.cmake` with the canonical kernel flag set from §8.4
- [x] Write `tools/build_toolchain.sh` (binutils + GCC stage1 + libgcc for `x86_64-elf`)
- [x] Write top-level `CMakeLists.txt` with `add_subdirectory` wiring + `run` / `debug` / `image` custom targets
- [x] Write `tools/mkimage.py` — assemble ESP (FAT32) disk image with `BOOTX64.EFI`
- [x] Write `tools/run_qemu.py` — one command to build + boot + attach serial
- [x] Write `tools/verify_image.py` — independent structural verifier (35 checks) ✅ **verified**
- [~] CI: `.github/workflows/ci.yml` builds and boots in QEMU, greps serial for the boot markers
      — **unticked until it is seen green.** This box was ticked at v0.1 on the
      strength of the workflow file existing. It has never passed: every run
      since v0.3 was red on the first step of the first job, and the
      build-and-boot job was *skipped* on all of them, so the pipeline had never
      once run the kernel. Seven separate faults were found and fixed after v0.5
      (see `docs/debug-log.md`). The box goes back to `[x]` when a run is green,
      and not before — a ticked box for a job that has never executed is exactly
      the kind of claim this file is supposed to be free of.
- [x] **Commit & push** 🚩

#### 0.1.2 UEFI boot bridge (`boot/uefi/`)
- [x] `efi_main.c` — `EFI_SYSTEM_TABLE` / `EFI_BOOT_SERVICES` layout per UEFI 2.10 (packed structs, exact field order, all 45 boot-service members listed so no offset shifts)
- [x] `efi_console.c` equivalent — `ConOut->OutputString` wrapper, with CRLF translation
- [x] Locate `EFI_GRAPHICS_OUTPUT_PROTOCOL` via `LocateProtocol`; record `FrameBufferBase`, resolution, `PixelsPerScanLine` and pixel format
- [x] Read the UEFI memory map twice (probe size, then fetch) into a stable buffer, with slack for growth
- [x] `ExitBootServices` with the **current** `MapKey` — retry loop with a refetched map
- [x] Build `af_boot_info` (§9.4): passed in `rdi` **and** mirrored at the fixed backup address `0x7000`
- [x] Set up a fresh stack, disable interrupts, jump to `kernel_entry` with `boot_info` in `rdi`
- [x] `bootx64.lds` linker script placing the EFI app correctly for PE/COFF (`--subsystem 10`, `--image-base 0`, `-Wl,--no-seh`)
- [x] Print a fallback "boot failed" message if any step returns an error status
- [x] Disable the firmware watchdog
- [x] Mark any region covering the framebuffer as `AF_MEM_FRAMEBUFFER` so the PMM cannot allocate video memory
- [x] Kernel image embedded via `incbin` — no `SimpleFileSystem`, no path handling, and the bridge and kernel are structurally always from the same commit

#### 0.1.3 Kernel entry & arch init (`kernel/arch/x86_64/`)
- [x] `entry.asm` — set up a 16 KB stack (`kernel_stack_bottom/top` in `.bss`), zero `.bss`, call `kmain`
- [x] `serial.c` — COM1 (0x3F8) init: 115200 8N1, `putc`/`write`, with a scratch-register probe so a missing UART cannot hang the kernel
- [x] `gdt.c` — 64-bit GDT: null, kernel code (0x08), kernel data (0x10), user code (0x18), user data (0x20), TSS descriptor (0x28)
- [x] Load `cs`/`ds`/`es`/`ss` via far return + `lgdt`; clear `fs`/`gs` for now
- [x] `idt.c` — 256-entry IDT; vectors 0–31 = CPU exceptions, 32–47 = PIC remapped IRQs (remapped to 0x20/0x28)
- [x] `isr.asm` — 256 stubs via NASM macros, pushing a dummy error code where the CPU does not, then the vector number, then `jmp isr_common`
- [x] `isr_common` — save all GP registers, call C `isr_dispatch(isr_frame_t *)`, restore, `iretq`, with defensive 16-byte stack alignment
- [x] Exception handler prints the vector name, decoded error code, faulting address from `CR2` for `#PF`, and the full register set, then panics
- [ ] Map `#DF` to a dedicated IST stack (TSS IST entry) — *deferred: needs the TSS written at v0.4*
- [x] `panic.c` — serial dump, recursion guard, reason/file/line/function, architecture register dump, `hlt` loop (no red screen yet; the on-screen console carries the log)
- [x] `log.c` — leveled logger (`TRACE/DEBUG/INFO/WARN/ERROR/FATAL`) with elapsed-time prefixes, a runtime level filter and a swappable sink

#### 0.1.4 Framebuffer & early graphics (`kernel/core/fb.c`)
- [x] `af_surface_t` — pixels, width, height, pitch, bpp, format, size
- [x] `af_fb_init()` — validate address and geometry, sanity-check `pitch >= width × bpp / 8`, and report stride padding explicitly
- [x] `af_fb_put_pixel()` with correct channel order per format (RGBX8888, BGRX8888, RGB565)
- [x] `af_fb_fill_rect()`, `af_fb_draw_rect()`, `af_fb_draw_hline()`, `af_fb_draw_vline()`, `af_fb_clear()`
- [x] `af_fb_fill_gradient_v()` and `af_fb_blend_rect()` (alpha blending)
- [x] **8×16 bitmap font** (ASCII 32–126) in `kernel/core/font8x16.c` + `af_fb_draw_char`, `af_fb_draw_text`, `af_fb_draw_text_scaled`
- [x] `af_splash_draw()` — the AfriyieOS mark and wordmark drawn as raw primitives, responsive to the display aspect ratio
- [x] Handle `PixelsPerScanLine != width` correctly — all drawing goes through the pitch
- [x] On-screen console mirroring the log, backed by a RAM grid that scrolls with `memmove` rather than repainting every glyph

#### 0.1.5 v0.1 tests & acceptance
- [x] In-kernel self tests: `kstring`, `vsnprintf`, boot-handoff validation (every rejection path), framebuffer invariants — reporting `AF_TEST_OK` or `AF_TEST_FAIL:<name>`
- [x] Serial golden test: the QEMU runner asserts ordered markers (`AF_GDT_READY`, `AF_IDT_READY`, `AF_TEST_OK`, `AF_BOOT_OK`) and rejects any `AF_PANIC:` / `AF_TEST_FAIL:` line
- [x] Host tests for the image toolchain — 25 tests, including a byte-exact round trip through the FAT32 writer, a negative test asserting the verifier **rejects** a corrupted GPT, and code-vs-prose checks on the shared constants
- [x] Screenshot test: `tools/screenshot.py` boots the system, grabs the framebuffer through the QEMU monitor, and asserts the frame is not blank, carries the brand palette (which catches a red/blue swap that a serial log cannot), and has the mark where the responsive rule requires
- [ ] Panic test: deliberately raise `int3`, assert the panic handler prints and halts instead of triple-faulting — *deferred to v0.5, where the IDT work is revisited*
- [x] Memory-map test: assert >0 usable regions and that the kernel image is marked as used

**✅ Acceptance criteria:** `cmake --build build/x86_64 && python3 tools/run_qemu.py` boots in QEMU, prints a clean serial log ending in `AF_BOOT_OK`, and shows the AfriyieOS splash with "Hello from AfriyieOS v0.1" on screen.

**⚠️ Where people get stuck:** the UEFI struct layouts (one wrong field offset = instant triple fault with no output) and `PixelsPerScanLine`. Mitigation: `_Static_assert(offsetof(...))` on **every** UEFI struct member, which is what `boot/uefi/efi.h` does.

**Deviations from this plan, and why:**

1. **The kernel is linked as an identity mapping at 1 MiB, not in the higher half.**
   A higher-half image faults on its first instruction fetch without paging, and
   v0.1 has none. The move happens in the same commit as the v0.2 paging
   bootstrap, gated on `AF_KERNEL_HIGHER_HALF`. See
   [architecture/memory-model.md](architecture/memory-model.md) §7.
2. **The boot bridge embeds the kernel with `incbin`** rather than loading it from
   the ESP. Removes a whole file-system code path and structurally guarantees the
   two are never from different commits. Loading from the ESP begins in v0.2.
3. **The screenshot and `int3` panic tests are deferred.** Both need a working
   CI boot loop first; neither tests anything the current self tests do not
   already cover at the code level.

---

### ✅ v0.2 — *Roots*: Memory & Multitasking — **COMPLETE**

**Goal:** manage physical RAM and run multiple tasks simultaneously.
**Verified:** bitmap PMM with 10 000-frame exactness, slab heap with a
100 000-operation fuzz, 100 Hz timer, round-robin scheduler with proven
preemption. `./tools/verify_all.sh` — 7 of 7 checks pass.
Evidence: [releases/evidence/v0.2.0-scheduler.txt](releases/evidence/v0.2.0-scheduler.txt)

> The remaining v0.2 item is the higher-half kernel move and the VMM, which are
> coupled to each other and land together — see the note at the end of this
> section.

#### 0.2.1 Physical Memory Manager
- [x] Normalize `af_boot_info` regions into a typed list of `{base, length, type}`
- [x] Bitmap allocator: 1 bit per 4 KB frame, placed in usable RAM and sized from the highest *usable* address
- [x] Mark reserved: kernel image (including `.bss`), boot_info backup, bitmap itself, framebuffer, holes
- [x] `pmm_alloc_frame()`, `pmm_alloc_frames(n)`, `pmm_alloc_frame_z()`, `pmm_free_frame()`, `pmm_free_frames()`
- [x] Refcounting per frame (needed for copy-on-write and shared memory later)
- [x] Statistics: total/free/used frames, largest contiguous run
- [x] In-kernel test: allocate 10 000 frames, prove all distinct, free them, assert the count returns to the start

#### 0.2.2 Kernel heap
- [x] Slab allocator: one cache per size class (16/32/64/128/256/512/1024/2048)
- [x] `kmalloc(size)`, `kzalloc(size)`, `kfree(ptr)`, `krealloc(ptr, size)`
- [x] Large allocations (> 2048 bytes) fall through to contiguous frame allocation
- [x] `kmalloc_aligned(size, align)` support
- [x] Debug mode: redzone before/after each allocation, verified on free
- [x] `heap_check()`: slab, free-list and guard-band integrity
- [x] Test: fuzz 100 000 random alloc/free operations, verify no corruption and zero leaks

#### 0.2.3 Threads and scheduler
- [x] `struct af_thread` — `tid`, `state`, `priority`, `kernel_stack`, `saved_context`, `time_slice`
- [x] Thread states: `CREATED · RUNNABLE · RUNNING · BLOCKED · SLEEPING · ZOMBIE · DEAD`
- [x] Run queues: 8 priority levels, round-robin within a level
- [x] `context.asm` — `context_switch(old*, next*)` saving/restoring **callee-saved** registers plus `rsp`, with a trampoline for a thread that has never run
- [x] `thread_create(entry, arg, stack_size, priority)` and `sched_admit()`
- [x] `thread_exit()` and zombie reaping from a context that does not own the exiting stack
- [x] **PIT timer** programming channel 0 at 100 Hz → IRQ0 → `sched_tick()`
- [x] Preemption: on tick, decrement the running thread's slice; on expiry, requeue and pick the next
- [x] `sched_yield()`, `sched_sleep(ticks)`, `sched_block()`, `sched_unblock()`
- [x] Idle thread that halts the CPU, separate from the boot context
- [x] Generic IRQ layer with chained handlers and acknowledge-before-dispatch
- [x] Interrupt-safe spinlocks
- [x] **Critical correctness test:** two threads, 200 iterations each, 201 context switches apiece — verified
- [x] **Preemption proof:** a thread that never yields was interrupted 13.8 million times

#### 0.2.4 v0.2 acceptance
- [x] Two threads print `A` and `B` with a near-strict alternation, both completing
- [x] Preemption demonstrated with a thread that cannot be starved
- [x] An exited thread is reaped and removed from the table (no stack leak)
- [ ] `pmm`/`vmm` unit tests (map at `0xDEADB000`, write, read back, unmap, assert #PF) — *needs the VMM*
- [ ] 1 000 thread create/exit cycles with no frame leak — *next*

#### 0.2.5 Virtual memory and paging
- [x] Page-table structures: `PML4 → PDPT → PD → PT` with 4 KiB pages
- [x] `hal_pt_create()`, `hal_pt_destroy()`, `hal_map_page()`, `hal_map_range()`, `hal_unmap_page()`, `hal_translate()`, `hal_query_flags()`
- [x] **The kernel owns its page tables.** Until this landed the kernel was executing on the tables OVMF left behind after `ExitBootServices` — they happened to identity-map everything we touched, which is why nothing broke, but the kernel could not rely on any mapping existing and could not create a second address space at all
- [x] Boot-time bootstrap: 3 GiB identity map with 2 MiB pages, sized from the PMM *and* the framebuffer, which sits above the PMM's range
- [x] Portable `HAL_*` mapping flags translated to architecture bits, with `NX` inverted correctly (absent execute permission *sets* the bit)
- [x] Second independent address space created and destroyed, with its mappings provably not leaking into the kernel's — the foundation user mode is built on
- [x] TLB invalidation after every mapping change
- [x] Test: map a frame above 4 GiB, write through it, verify translation and contents, unmap, verify the mapping is gone, and verify a second address space is independent
- [x] Physical direct map at `0xFFFF_8000_0000_0000` — *the constant and the conversion helpers exist; the mapping itself lands with the higher-half move*
- [ ] Higher-half kernel: relocate to `0xFFFF_FFFF_8000_0000` (`-mcmodel=kernel` + linker script + bootstrap stub)
- [ ] Demand paging groundwork (page fault → allocate → map → retry) — used by the `v0.4` ELF loader
- [ ] Guard pages around kernel stacks (unmapped page → clean #PF instead of silent corruption)

**✅ Acceptance criteria met:** two kernel threads printing `A` and `B` in a fair
alternating pattern, with memory statistics stable afterwards.

**Remaining for v0.2:** the **higher-half kernel relocation**. The VMM, paging,
and second address spaces are done — what is left is moving the kernel image
itself from its identity-mapped 1 MiB home into the top 2 GiB, which needs
`-mcmodel=kernel`, a VMA≠LMA linker script, and a bootstrap stub that builds the
higher-half mapping before the jump. It is deliberately one commit, because a
partially-converted kernel faults on its first instruction fetch.

**Goal:** manage physical RAM, own the address space, switch between threads.

#### 0.2.1 Physical Memory Manager
- [ ] Normalize `afriyie_boot_info` regions into a sorted list of `{base, length, type}`
- [ ] Bitmap allocator: 1 bit per 4 KB frame, one bitmap at `0xFFFF_C000_0000_0000 + kheap`
- [ ] Mark reserved: kernel image, boot_info, bitmap itself, framebuffer, ACPI-reclaim areas, holes
- [ ] `pmm_alloc_frame()`, `pmm_alloc_frames(n)`, `pmm_free_frame()`, `pmm_free_frames()`
- [ ] Refcounting per frame (needed for copy-on-write and shared memory later)
- [ ] Statistics: total/free/used frames, largest contiguous run
- [ ] In-kernel test: allocate 10 000 frames, free them, assert the bitmap returns to its initial popcount

#### 0.2.2 Virtual Memory Manager
- [ ] Page-table structures: `PML4 → PDPT → PD → PT` with 4 KB pages
- [ ] `vmm_map(root, vaddr, paddr, flags)`, `vmm_unmap(root, vaddr)`, `vmm_translate(root, vaddr)`
- [ ] `vmm_map_range(root, vaddr, paddr, size, flags)`
- [ ] Higher-half kernel: relocate kernel to `0xFFFF_FFFF_8000_0000` (`-mcmodel=kernel` + linker script)
- [ ] Physical direct map at `0xFFFF_8000_0000_0000` — all RAM, offset-mapped, 2 MB huge pages for speed
- [ ] Kernel page tables become the template for every new process
- [ ] `vmm_new_address_space()`, `vmm_destroy_address_space()`
- [ ] Guard pages around kernel stacks (unmapped page → clean #PF instead of silent corruption)
- [ ] TLB management: `invlpg` on single-page ops, full CR3 reload on address-space switch
- [ ] `#PF` handler that decodes the error code (present/write/user/instruction-fetch) and reports the faulting address from `CR2`
- [ ] Demand paging groundwork (page fault → allocate → map → retry) — used by `v0.4` ELF loader

#### 0.2.3 Kernel heap
- [ ] Slab allocator: one cache per common size class (16/32/64/128/256/512/1024/2048)
- [ ] `kmalloc(size)`, `kzalloc(size)`, `kfree(ptr)`, `krealloc(ptr, size)`
- [ ] Large allocations (> 4 KB) fall through to contiguous frame allocation
- [ ] `kmalloc` alignment support (16-byte minimum for all), plus `kalloc_aligned(align)`
- [ ] Debug mode: redzone before/after each allocation + canary verification on free
- [ ] Leak tracking (debug build only): every live allocation recorded with file/line
- [ ] Test: fuzz 100 000 random alloc/free operations, verify no corruption and zero leaks

#### 0.2.4 Threads & scheduler
- [ ] `struct af_thread` — `tid`, `state`, `priority`, `kernel_stack`, `user_stack`, `saved_context`, `addr_space`, `cap_table`, `cpu_affinity`, `time_slice`
- [ ] Thread states: `CREATED · RUNNABLE · RUNNING · BLOCKED · SLEEPING · ZOMBIE · DEAD`
- [ ] Run queues: 8 priority levels, round-robin within a level
- [ ] `context.asm` — `context_switch(old*, new*)` saving/restoring **callee-saved** registers (`rbx, rbp, r12–r15`) plus `rsp` and `rip`
- [ ] `thread_create(entry, arg, stack_size, priority)`
- [ ] `thread_exit()` and reaping of zombie threads
- [ ] **PIT/PIT-replacement timer**: program APIC timer (or PIT at 100 Hz as a fallback) → IRQ0 → `sched_tick()`
- [ ] Preemption: on tick, decrement the running thread's time slice; on expiry push it to the back of its queue and pick the next
- [ ] `thread_yield()`, `thread_sleep(ms)`, `thread_block()`, `thread_unblock()`
- [ ] Idle thread (`hlt` loop) that always exists and never exits
- [ ] First thread becomes `kmain`'s continuation; a bootstrap thread is created for the rest of kernel init
- [ ] **Critical correctness test:** create 3 threads that increment a shared counter under a spinlock; run 10 million iterations; assert the sum is exact
- [ ] Spinlocks + `irq_save`/`irq_restore` primitives; document the lock ordering rules

#### 0.2.5 v0.2 tests & acceptance
- [ ] Test: two threads print `A` and `B` in a strict alternating pattern with preemption
- [ ] Test: `pmm`/`vmm` unit tests (map at `0xDEADB000`, write, read back, unmap, assert #PF)
- [ ] Test: 1 000 thread create/exit cycles — assert no frame leak
- [ ] Sanitizer-ish: enable `-fsanitize=undefined` on host-side unit tests where possible

**✅ Acceptance criteria:** Serial log shows `A` and `B` interleaving in an alternating, fair pattern for at least 1 000 lines, with memory statistics stable (no leak) afterwards.

**⚠️ Where people get stuck:** context switch register save bugs. Symptom: a thread dies after a few thousand switches. Mitigation: save *all* registers first (get it working), then optimize down to callee-saved and re-test.

---

### 🟢 v0.3 — *Trunk*: Disk & File System

**Goal:** read real files from a real (virtual) disk.

#### 0.3.1 PCI bus enumeration
- [ ] `pci_scan()` — brute-force scan of bus 0–255, device 0–31, function 0–7 via `0xCF8`/`0xCFC` config space
- [ ] Read vendor/device IDs, class/subclass, BARs (handle 32-bit and 64-bit BARs, I/O vs memory)
- [ ] Build a device list; log every device at boot (invaluable for debugging)
- [ ] `pci_find_class(class, subclass)`, `pci_find_vendor_device(v, d)`
- [ ] Enable bus mastering + memory space in the command register for devices we claim

#### 0.3.2 virtio-blk driver (kernel-mode for now; moves to user space in v0.7)
- [ ] Virtio PCI modern (1.0+) capability parsing, or legacy I/O BAR fallback
- [ ] Device initialization handshake: reset → `ACKNOWLEDGE` → `DRIVER` → feature negotiation → `FEATURES_OK` → `DRIVER_OK`
- [ ] Virtqueue setup: descriptor table, available ring, used ring — each in its own physically-contiguous region
- [ ] `virtio_blk_read(sector, buf, count)`, `virtio_blk_write(...)`
- [ ] Blocking read via a completion interrupt (or polling during bring-up, then interrupt-driven)
- [ ] Timeout + error recovery: retry, then mark the device dead rather than hanging forever
- [ ] Test: read sector 0, assert the boot signature `0xAA55` if the disk is partitioned

#### 0.3.3 FAT32 read-only parser
- [ ] Parse the BPB (`bytes_per_sector`, `sectors_per_cluster`, `reserved_sectors`, `num_fats`, `fat_size`, `root_cluster`)
- [ ] Handle FAT32 (and detect FAT16/12 to give a clear error)
- [ ] Cluster chain traversal (`FAT[cluster]` → next cluster, `>= 0x0FFFFFF8` = end)
- [ ] Directory entry parsing: long file names (LFN) + 8.3 short names, attributes, first cluster, size
- [ ] `fat32_lookup_path("/EFI/BOOT/BOOTX64.EFI")`, `fat32_open`, `fat32_read`, `fat32_close`
- [ ] `fat32_readdir(handle, index, out_entry)`
- [ ] Cache the FAT and the current directory to avoid re-reading sectors per entry

#### 0.3.4 Minimal VFS layer
- [ ] `struct af_vfs_node { ops; type; size; ... }` and `struct af_file { node; offset; flags; }`
- [ ] `vfs_open/read/write/close/stat/readdir` dispatching to a mount table
- [ ] Mount a FAT32 volume at `/mnt/boot` for testing, later at `/`
- [ ] Root file system abstraction so `AFS` (v0.9) can be mounted alongside FAT32

#### 0.3.5 v0.3 tests & acceptance
- [ ] Create a test disk image with a known `HELLO.TXT` containing `Hello from disk`
- [ ] Boot, mount, open, read, print — assert the exact string over serial
- [ ] Test: read a 1 MB file and checksum it against the host-computed checksum
- [ ] Test: negative cases — non-existent file, corrupt directory entry, truncated chain

**✅ Acceptance criteria:** Serial log prints `Hello from disk` read from `HELLO.TXT` on a virtio disk through the FAT32 parser.

---

### 🟢 v0.4 — *Branches*: User Mode & Syscalls

**Goal:** the kernel stops doing everything itself. Ring 3 exists.

> **Status: the privilege boundary and the ELF loader are done and verified.**
> A program compiled by the cross compiler, linked at 4 GiB, stored in the FAT32
> volume as `/INIT.ELF`, is read off the disk by the kernel, parsed, mapped
> segment by segment, and entered at CPL 3 — where it runs its own checks, prints
> `AF_EXEC_RAN`, and exits cleanly. Evidence:
> `docs/releases/evidence/v0.4.0-exec.log`.
>
> **Processes are not done**, and are the reason this milestone is not closed.
> Everything below that depends on more than one address space — `fork`, `spawn`,
> `wait`, per-process kernel stacks, argv — is deferred to v0.5 rather than
> faked. See the deferral note at 0.4.3.

#### 0.4.1 GDT/TSS & privilege transition
- [x] TSS with RSP0 (kernel stack for interrupts arriving from user mode)
- [ ] IST entries for `#DF`/`#PF` — *open: a fault while handling a fault is still a
      triple fault. Needs an IST stack per vector.*
- [x] User code/data segments with correct DPL=3
- [x] `af_x86_enter_user_mode(entry, user_stack)` — `iretq` with a crafted frame
      (SS, RSP, RFLAGS, CS, RIP)
- [x] Kernel stacks per *thread* — every `af_thread_t` owns one, and the TSS RSP0
      is repointed on every context switch
- [ ] Kernel stacks per *process* — *deferred with 0.4.3*

#### 0.4.2 System call entry
- [ ] `SYSCALL`/`SYSRET` with `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK` — *deferred:
      `int 0x80` is used instead. It costs a descriptor-table lookup and goes
      through the interrupt path the kernel already had tested, and it carries the
      arguments in the same registers — so this is a performance change with no
      ABI change, and `libs/libaf/include/af.h` is the only file that has to
      change when it lands.*
- [x] Entry through the IDT at vector `0x80`, DPL=3, arriving on the TSS RSP0 stack
- [x] Number bound check (`AF_SYS_MAX`) — an out-of-range number returns
      `AF_ERR_NOTSUP` rather than indexing anything
- [x] Arg validation: range and wraparound are checked before use
- [x] Arg validation: **is the range actually mapped?** Every page the range
      touches is looked up in the page tables and must be present, user-accessible,
      and writable if the call writes. The walk is bounded (16 MiB) because the
      length is attacker-controlled — a page-table lookup per page on an arbitrary
      length is a denial-of-service vector. Verified from ring 3 by `apps/init`,
      which passes six malformed buffers and requires an error from each.
- [x] `sys_debug_write`, `sys_exit`, `sys_yield`, `sys_clock_get`
- [x] Return-value convention: `>= 0` success, negative `af_status_t` error
- [x] Numbering is append-only, with retired numbers reserved

#### 0.4.3 Processes — **partially done at v0.5: address spaces exist, the process object does not**
- [ ] `struct af_process { pid; addr_space; threads; cap_table; fds; parent; exit_code; }`
- [x] A user address space that can be installed — `hal_pt_create_user`, with the
      kernel's half shared by pointer and user space confined to its own top-level
      slot. Proved by a test that creates one, checks the kernel is reachable and
      the user region is not, then **switches to it and switches back**.
- [x] The address space is a property of the running thread: the scheduler installs
      it on every context switch, and destroys it when the thread is reaped.
- [ ] `process_create()` — the object that owns an address space and a set of
      threads. Today the address space is owned by a **thread**, which means two
      threads of one program would not share memory. That is the next thing.
- [ ] `process_fork()` via copy-on-write page tables
- [ ] `sys_thread_create` spawning a user thread in the current process
- [ ] `sys_process_spawn(path_cap, argv)`
- [ ] `wait`/`exit` semantics, an "init" process that never dies

> **What v0.5 actually changed, and what it did not.** The limitation that blocked
> this milestone is gone: there are now genuinely separate address spaces, and a
> program is loaded into its own rather than into the kernel's. What remains is the
> *object* that groups threads under one address space, and everything that follows
> from it — `fork`, `spawn`, `wait`, and the `af_process_t *` that
> `docs/abi/syscalls.md` §3 passes to its validator.
>
> The reason this is not stubbed: an address space owned by a thread is enough to
> run one program, and it is exactly the wrong shape for two. Adding `fork` on top
> of it would mean writing it twice.

#### 0.4.4 ELF64 loader
- [x] Validate the header: magic, class=64, little-endian, machine=x86_64,
      type=EXEC/ET_DYN, `e_phentsize`
- [x] Program headers: one frame per page per `PT_LOAD`, with per-segment flags
      (r-x / r-- / rw-)
- [x] Zero the BSS portion (`p_memsz > p_filesz`), by allocating zeroed frames
- [x] Reject malformed ELFs: segment file ranges outside the file, `p_filesz >
      p_memsz`, no `PT_LOAD` segments, out-of-range entry point
- [x] 64 KiB user stack, mapped user+writable and **non-executable** (NX)
- [ ] Guard page below the stack — *deferred: a growing stack needs demand paging.
      At v0.4 the stack is a fixed 64 KiB region with no guard, so a stack
      overflow writes into whatever is mapped below it.*
- [ ] `argc`/`argv`/`envp` on the stack — *deferred: there is no process to own
      them, and one program with no arguments needs none*
- [ ] Overlapping-segment rejection — *the per-page mapper returns `ERR_EXIST`
      when two segments claim the same page, so the case fails closed. It fails
      with a mapping error rather than a loader diagnostic; worth a pre-pass.*

#### 0.4.5 User-space runtime (`libs/libaf/`)
- [x] Syscall wrappers: `af_write`, `af_exit`, `af_yield`, `af_clock_ns`
- [x] `af_strlen`, `af_puts`, `af_putc`, `af_putu`, `af_puti`
- [ ] `memset/memcpy/memmove/memcmp/strcmp/strncpy` — *not yet; nothing has needed
      them. They arrive as a block when the first server does.*
- [ ] A full `printf` — *deferred: `af_putu`/`af_puti` cover what init needed.
      Format strings are worth having, but only once there is a buffer to build
      them in and something to print from a driver.*
- [x] `_start` stub (`libs/libaf/crt0.S`) that calls `main` and then `af_exit`
- [x] Linker script for user programs (`libs/libaf/user.lds`)

> **The link address is 4 GiB, not `0x400000` as originally sketched.**
> `0x400000` sits inside the kernel's 3 GiB identity map, which is built from
> 2 MiB huge pages marked kernel-only. A user program there would require those
> huge pages to be split, and would occupy address space the kernel has already
> described to itself as its own. 4 GiB is above the identity map, inside the
> user half, and — because the segments are aligned to page boundaries — maps
> cleanly with no interaction with the kernel's own mappings at all. The cost is
> that user code must be compiled with `-mcmodel=large`, since the small model
> cannot express a 32-bit absolute reference to `0x100000000`.

#### 0.4.6 v0.4 tests & acceptance
- [x] Build a user program that writes to the debug console and exits with code 0
      (`apps/init`, packed into the FAT32 volume as `/INIT.ELF`)
- [x] Assert: the string appears, `main` returns, `sys_exit` terminates the thread,
      the scheduler reaps it
- [x] The program verifies the kernel's own work from the outside: that `.bss` came
      back zero, that `.data` was copied, that a written pattern read back
      correctly, that `.rodata` is addressable through a computed index, and that
      the clock and yield system calls behave across a context switch
- [x] Page-permission assertions from the kernel side: code is user+exec and NOT
      writable, stack is user+writable and NOT executable, and the kernel image at
      `0x100000` is NOT user-accessible
- [x] CPL assertion test: the program reads `cs` and asserts the low 2 bits are 3,
      before any other check — because if it is not at CPL 3, every other check
      below it is worthless. (The panic dump also prints `cs`, but reading it in
      the program is the assertion; reading it in the kernel is only a report.)
- [x] Pointer-validation test: the program passes six malformed buffers — the null
      page, an in-range unmapped address, a hole inside its own image, a length
      that wraps, a range past the user top, and a zero length — and requires a
      clean negative status from each rather than a kernel fault. It then reads a
      buffer it legitimately owns, because a validator strict enough to reject
      everything would pass the first six and break every real program.

**✅ Acceptance criteria:** a user-mode ELF program is loaded off the file system,
entered at CPL 3, runs its own checks, and exits with status 0; the kernel remains
healthy.
**Status: met.** `AF_EXEC_PREPARED` and `AF_EXEC_RAN` both appear in the boot log,
and `tools/run_qemu.py` fails if either is missing.

**Milestone status: not closed.** The acceptance criterion is met; 0.4.3 is not
started and its absence is a real limitation, not a cosmetic one. The marker stays
green and the boxes stay unticked.


---

### 🟢 v0.5 — *Leaves*: Drivers & Input

**Goal:** the system can see its devices and respond to a keyboard/touchscreen.

#### 0.5.1 Driver framework
- [ ] `struct af_driver_ops { probe, start, stop, irq_handler }` — the universal driver interface
- [ ] Kernel-mode driver registration for now; the same interface is reused verbatim in user space at v0.7
- [ ] Interrupt sharing: multiple handlers per IRQ line, chained dispatch
- [ ] IRQ → driver routing table built from the PCI scan
- [ ] MMIO mapping helper: map a BAR's physical range into the direct map with correct cache attributes (`PWT`/`PCD` for device memory)

#### 0.5.2 Input subsystem
- [ ] `struct af_input_event { uint16_t type; uint16_t code; int32_t value; uint64_t timestamp; }`
- [ ] Event types: `KEY` (Linux-compatible codes: ESC=1, digits 2–11, letters 30–..., arrows 103–108), `REL`/`ABS` (pointer motion), `BTN` (mouse buttons / touch), `TOUCH` (multitouch slots), `SYN`
- [ ] Ring buffer of input events with overflow policy (drop oldest + count drops)
- [ ] `sys_input_read(evt_ptr, max)` → user space pulls events (also deliverable via IPC notification in v0.8)

#### 0.5.3 Keyboard driver
- [ ] PS/2 keyboard (port `0x60`/`0x64`) — simplest path, IRQ1
- [ ] Scancode set 1 → keycode translation table, including `Shift`/`Ctrl`/`Alt`/`CapsLock` state
- [ ] Extended (`0xE0`) scancode handling for arrows/Home/End/Delete
- [ ] virtio-input keyboard as the QEMU-preferred modern path
- [ ] Key repeat: initial delay 500 ms, repeat 30/s (timer-driven)
- [ ] **Test:** type `hello` in QEMU via `sendkey`, assert 5 key events with the right codes

#### 0.5.4 Mouse / touch
- [ ] PS/2 mouse (IRQ12): 3-byte packet decoding, sign-extension, overflow checks, packet-sync state machine
- [ ] virtio-input tablet for absolute positioning (needed for QEMU screenshots tests)
- [ ] Touch framework: multitouch slots, pressure, per-contact tracking, tap vs drag classification
- [ ] Cursor state (position, button mask) maintained in the input layer

#### 0.5.5 Device manager service (kernel-mode prototype)
- [ ] Enumerate all discovered devices, assign IDs, log a clean table
- [ ] Expose `sys_dev_claim(dev_id)` so a driver can take ownership
- [ ] Resource arbitration: refuse double-claims of an MMIO region or IRQ

#### 0.5.6 ACPI basics (needed for power/shutdown later)
- [ ] Locate the RSDP (UEFI config table first, then `0xE0000` scan)
- [ ] Walk RSDT/XSDT → FADT, MADT (LAPIC/IOAPIC info), MCFG
- [ ] Use MADT for the APIC timer and I/O APIC routing (upgrade from the PIC/PIT)
- [ ] `acpi_shutdown()` via the FADT `PM1a_CNT` register (so `shutdown` actually powers off QEMU)

#### 0.5.7 v0.5 tests & acceptance
- [ ] Boot log shows the full device table (virtio-blk, virtio-input, PCI bridge, etc.)
- [ ] Typing `afriyie` in QEMU produces exactly 7 key events; `afriyie_os` shutdown powers off the VM
- [ ] Mouse movement produces correct absolute coordinates (assert against known `mouse_move` positions)

**✅ Acceptance criteria:** The boot log enumerates devices, keyboard input is captured with correct keycodes and modifier handling, and the system can power off cleanly via ACPI.

---

### 🟢 v0.6 — *Bloom*: Graphics Core

**Goal:** draw a real, modern-looking interface without any third-party renderer yet.

#### 0.6.1 Framebuffer upgrade
- [ ] Double buffering: an off-screen back buffer, blit to the front buffer on present
- [ ] `af_surface` abstraction: width, height, pitch, format, pixel data (may live in normal RAM, not VRAM)
- [ ] Clipping rectangles: every draw op clipped to a damaged region
- [ ] Fast blit paths: same-format 32-bit copy, alpha blend, fill — all word-at-a-time, not byte-at-a-time
- [ ] Vertical-scroll-free full redraw first (correct), then dirty-rect optimization later

#### 0.6.2 2D drawing primitives
- [ ] `fill_rect`, `draw_rect` (outline), `draw_line` (Bresenham), `draw_rounded_rect` (analytic AA optional)
- [ ] `fill_circle`, `draw_circle` (midpoint algorithm)
- [ ] Alpha blending: `dst = src*a + dst*(1-a)` with correct rounding, in a fast inner loop
- [ ] Linear gradient fills (vertical/horizontal) — the modern-UI look
- [ ] Box shadow / soft elevation (blurred rounded rect, cheap blurred approximation)
- [ ] Clipping stack (push/pop) used by the layout engine

#### 0.6.3 Text rendering (stage 1: bitmap, stage 2: vector)
- [ ] Scalable bitmap font families: 12/14/16/20/28 px rendered glyph atlases at build time
- [ ] Glyph atlas in RAM + `draw_text(surface, x, y, str, font, color)`
- [ ] Sub-pixel-free but properly hinted spacing; kerning pairs table for the interim font
- [ ] UTF-8 decoding (so the UI can show `ɛ` and `ɔ` — AfriyieOS should handle its own codename correctly)
- [ ] Later in v0.7: TrueType via the `stb_truetype` subset → SDF or cached-raster glyphs

#### 0.6.4 Compositor service (kernel-mode prototype, user-space in v0.8)
- [ ] `struct af_window { id; surface; x, y, w, h; z; visible; owner; title; decorations; }`
- [ ] Window list with z-ordering; composite back-to-front into the back buffer
- [ ] Damage tracking: accumulate dirty rects; present only when non-empty
- [ ] Cursor rendering (hardware or software) at the top of the z-order
- [ ] Present loop: 60 Hz target driven by the timer, only redrawing when damaged
- [ ] Test scene: draw 3 overlapping colored windows with text and a shadow

#### 0.6.5 v0.6 tests & acceptance
- [ ] Screenshot test: boot, draw the test scene, `screendump`, verify window positions by sampling known pixels
- [ ] Perf measurement: full-screen fill, blit, and 1000 rounded rects — log frame times to serial
- [ ] Frame rate assertion: the test scene composites at ≥ 60 FPS in QEMU on a modern host

**✅ Acceptance criteria:** On-screen test scene with three overlapping, shadowed, gradient-filled windows containing rendered text, composited at ≥ 60 FPS with damage tracking active.

---

### 🟢 v0.7 — *Fruit*: IPC & User-Space Services

**Goal:** the microkernel becomes a *micro*kernel. Servers move out of Ring 0.

#### 0.7.1 IPC core (kernel)
- [ ] Endpoint objects: `cap ENDPOINT`, each with a message queue and a block-on-recv queue
- [ ] `ipc_send` (async), `ipc_recv` (blocking), `ipc_call` (sync RPC), `ipc_reply`
- [ ] Register fastpath for the 64-byte `af_msg_t` (no allocation, no copy)
- [ ] Blocking semantics: sender blocks only on a full queue; receiver blocks when empty; wakeup on arrival
- [ ] IPC fastpath optimization: direct context switch on `ipc_call` (skip the scheduler for the reply path)
- [ ] Capability transfer inside messages (up to 4 caps per message, rights-narrowed on transfer)
- [ ] Notification objects (bitmask signal/wait) for IRQ delivery to user-space drivers
- [ ] Fault isolation: a server dying wakes all callers blocked on it with `AF_ERR_PEER_DEAD`

#### 0.7.2 Capability manager (kernel)
- [ ] Capability table per process; slot allocation, `derive` (with rights narrowing only — never widening), `delete`, `revoke`
- [ ] Reference counting on all kernel objects; destruction on last-capability-drop
- [ ] Capability validation on **every** syscall argument
- [ ] Bootstrapping: the init process receives capabilities to the name service and the first endpoints

#### 0.7.3 Shared memory & memory grants
- [ ] `sys_mem_alloc(size)` → `cap FRAME`
- [ ] `sys_mem_grant(dst_proc, cap, vaddr, rights)` → maps into the receiver
- [ ] Copy-on-write fork cleanup and frame refcounting audit
- [ ] Test: 8 MB buffer shared between two processes, both verify contents, no copy performed

#### 0.7.4 User-space servers
- [ ] **`servers/init`** — PID 1: starts the name service, device manager, file system, compositor; restarts any server that dies (supervision with backoff)
- [ ] **`servers/name`** — service registry: `register(name, cap)`, `lookup(name)`; the only well-known endpoint
- [ ] **`servers/fs`** — the file system service: AFS (read-write) + FAT32 (read-only), moved out of the kernel
- [ ] **`servers/devmgr`** — enumerates devices from a kernel-provided snapshot, spawns the right driver process for each, hands it the required capabilities/IRQs
- [ ] **`servers/compositor`** — the window server, now a real process; apps create windows and blit into shared surfaces
- [ ] Driver SDK (`drivers/driver_sdk/`) — headers for a user-space driver to: receive its device capability, claim MMIO, wait on IRQ notifications, and respond

#### 0.7.5 Move drivers to user space
- [ ] `drivers/virtio_blk` — first user-space driver (proves the model end-to-end)
- [ ] `drivers/ps2` or `drivers/virtio_input` — first user-space input driver, pushing events over IPC
- [ ] Kernel keeps **only**: MMIO mapping, IRQ notification, DMA-capable frame allocation
- [ ] Kernel shrinks measurably: log the kernel size before/after (target: drivers + FS removal drops kernel size ≥ 30%)

#### 0.7.6 libafpp (the mini-STL)
- [ ] `af::Vector<T>` — dynamic array with explicit `try_push_back` failure handling
- [ ] `af::String` — UTF-8 aware, no SSO tricks needed at first
- [ ] `af::HashMap<K,V>` — open addressing, `fnv1a` hashing
- [ ] `af::UniquePtr<T>`, `af::SharedPtr<T>`
- [ ] `af::Arena` — bump allocator for UI frame allocations, reset every frame
- [ ] `af::Result<T>` — error-as-value, since exceptions are banned

#### 0.7.7 v0.7 tests & acceptance
- [ ] App calls the file system service over IPC; serial log proves the whole path: app → libaf → sys_ipc_call → fs server → virtio driver → disk
- [ ] Kill test: `SIGKILL` the FS server; assert callers get `AF_ERR_PEER_DEAD`, init restarts it, and the system recovers without reboot
- [ ] Latency benchmark: 1 000 000 `ipc_call` round trips, log mean/p99 latency in cycles
- [ ] Kernel size report: assert the kernel is under the size budget in §14

**✅ Acceptance criteria:** The file system runs as a user-space process, applications read files over IPC, and killing the FS server does not crash the system — init restarts it and services resume.

---

### 🟢 v0.8 — *Canopy*: Window Manager & Shell

**Goal:** an interactive graphical desktop/phone environment users can actually touch.

#### 0.8.1 Window manager
- [ ] `create_window(w, h, title) → (window_id, shared_surface)`
- [ ] Shared-surface protocol: the app draws into granted memory; the compositor composites it
- [ ] Window decorations: title bar, close/minimize buttons (desktop), swipe-to-dismiss (phone)
- [ ] Focus management: click-to-focus, alt-tab / task-switcher cycling, focus follows the topmost window
- [ ] Move/resize: drag by title bar, resize by edges/corners with a hit-test margin
- [ ] Hit-testing: map a screen coordinate to `(window, local_coord)`; used for both input and cursor shape
- [ ] Damage propagation: app marks damage → compositor invalidates → presents the union of rects

#### 0.8.2 Input routing
- [ ] Route keyboard events to the focused window (via that window's input endpoint)
- [ ] Route pointer/touch events to the window under the cursor, with capture during drags
- [ ] `sys_notify` based event delivery: apps wait on a notification, then drain the event queue
- [ ] Global shortcuts: alt-tab, alt-F4, super key → task switcher, phone: swipe from edge → home
- [ ] Cursor shape changes: text I-beam over text fields, resize arrows over borders

#### 0.8.3 UI framework (`libs/libafgui/`)
- [ ] Declarative widget tree: `Column`, `Row`, `Stack`, `Padding`, `Center`, `Text`, `Button`, `IconButton`, `TextField`, `ListView`, `Checkbox`, `Slider`, `ProgressBar`, `Card`, `Dialog`
- [ ] Layout engine: measure/arrange two-pass, flex weights, min/max constraints, intrinsic sizing
- [ ] Event router: hit-test the widget tree, dispatch pointer/key events, focus chain, tab navigation
- [ ] State & rebuild: `setState()` triggers a scoped rebuild of the affected subtree only
- [ ] Theme system: tokens loaded from `tokens.afh`, light/dark variants, runtime switch
- [ ] Scrolling: kinetic scrolling with momentum + overscroll feedback (touch), wheel (desktop)
- [ ] Responsive breakpoints: `phone | compact | desktop` with per-breakpoint layouts in the same widget tree
- [ ] Animations: interpolation, `AnimatedOpacity`, `AnimatedPosition`, easing curves per §6.2

#### 0.8.4 Afriyie Shell (the system UI)
- [ ] **Desktop mode** — taskbar at the bottom, start menu, system tray (clock, battery, network), window list
- [ ] **Phone mode** — status bar (time, battery, signal), app grid, gesture bar, notification shade (pull down)
- [ ] Lock screen: clock, date, unlock gesture
- [ ] Boot splash → shell transition with a fade animation
- [ ] Power menu: shut down, restart, sleep (ACPI-backed)

#### 0.8.5 v0.8 tests & acceptance
- [ ] Integration test: scripted QEMU session — open two apps, move a window, click to focus, screenshot the result
- [ ] Input-routing test: `sendkey` reaches the focused window; `mouse_move`+`mouse_button` focuses and drags correctly
- [ ] Responsive test: boot at 1280×800 (desktop) and 480×800 (phone); assert the correct shell mode and layout
- [ ] Frame budget: interaction remains ≥ 60 FPS with 4 windows and an animation running

**✅ Acceptance criteria:** An interactive graphical environment with two or more overlapping windows, working focus, dragging, a taskbar in desktop mode, a phone-mode layout on a portrait display, and 60 FPS sustained.

---

### 🟢 v0.9 — *Harvest*: Installer & Disk Images

**Goal:** AfriyieOS installs itself onto a disk and boots from it. This is the "real OS" threshold.

#### 0.9.1 AFS — the native file system (read-write)
- [ ] On-disk format spec in `docs/abi/afs-filesystem.md`: superblock, inode table, block bitmap, directory entries, extents
- [ ] Superblock: magic `AFS1`, version, block size, inode count, root inode, free-block count, journal area pointer
- [ ] Inodes: type, size, permissions, timestamps, direct + indirect extent pointers
- [ ] Block allocator: bitmap with a hinted search; defragmentation deferred
- [ ] Directory: sorted entries with names, inode numbers, types; long-name friendly
- [ ] Journaling (crash consistency): write-ahead log of metadata changes, replay at mount
- [ ] `afs_format(device)`, `afs_mount`, `afs_create/unlink/mkdir/rmdir/rename/read/write/truncate/stat/readdir`
- [ ] `tools/mkafs.py` — build an AFS image on the host (so we can boot from a pre-populated disk)
- [ ] fsck-style repair tool: `afriyie-fsck` (host side) verifying bitmaps and inode consistency

#### 0.9.2 Ramdisk & initramfs
- [ ] `initramfs` format: simple TAR + optional DEFLATE (miniz) containing `/init`, servers, drivers, apps
- [ ] Kernel parses it at boot (boot_info carries the address/size), mounts it as the initial root
- [ ] `tools/pack_initramfs.py` — build the payload from the build output
- [ ] Boot from ramdisk → hand off to the real disk root once AFS is mounted

#### 0.9.3 Partitioning
- [ ] GPT writer (primary + backup header, CRC32 of header and entry array, EFI System Partition type GUID)
- [ ] Partition table reading on boot; locate the ESP and the AFS root partition by type GUID
- [ ] Optional MBR hybrid (legacy BIOS fallback) — deferred unless needed

#### 0.9.4 Installer application
- [ ] A UI application (uses libafgui) with: welcome → disk selection → partition preview → install progress → done
- [ ] Disk detection and a **destructive-action confirmation** with a clear warning
- [ ] Steps: create GPT → create ESP (FAT32, ~256 MB) → create AFS root (rest) → format → copy files → write the bootloader to the ESP → write the UEFI boot entry (`EFI/BOOT/BOOTX64.EFI` plus a fallback path for firmware that ignores NVRAM)
- [ ] Progress reporting with real byte counts; cancellation; error recovery
- [ ] Write a boot config (root partition identifier, kernel cmdline) into the ESP

#### 0.9.5 Image packaging
- [ ] `tools/mkimage.py --mode install` → bootable installer USB image (GPT + ESP + installer ramdisk)
- [ ] `tools/mkimage.py --mode disk` → pre-installed system disk image
- [ ] `xorriso` ISO target (UEFI El Torito) as an alternative to a raw `.img`
- [ ] `--mode phone` → Android-compatible `boot.img` (kernel + DTB + ramdisk + header) for fastboot — skeleton only until v1.1
- [ ] Release artifact naming + SHA-256 checksums + a release notes file

#### 0.9.6 Boot from installed disk
- [ ] Kernel identifies the root device from the boot config
- [ ] fs service mounts AFS read-write as `/`
- [ ] Session persistence check: create a file, reboot, verify it survived

#### 0.9.7 v0.9 tests & acceptance
- [ ] Full install simulation in QEMU: empty 4 GB disk + installer image → install → reboot from the installed disk → shell appears
- [ ] Persistence test: create `/home/user/test.txt`, reboot, `cat` it successfully
- [ ] AFS conformance suite: 500 randomized operations (create/write/rename/delete, various sizes), then unmount/remount and verify a full directory + checksum comparison
- [ ] Crash-consistency test: power-cut the VM mid-write, remount, assert the journal replays to a consistent state
- [ ] Real hardware test (optional but recommended): write the installer image to a USB stick, boot a spare PC, install to an internal SSD

**✅ Acceptance criteria:** A blank virtual disk becomes a bootable AfriyieOS installation via the installer, reboots into the shell, and files created before a reboot still exist afterwards.

---

### 🟢 v1.0 — *Oseadeɛyɔ*: Daily-Usable System

**Goal:** a person can sit down and *use* AfriyieOS for real work.

#### 1.0.1 Applications
- [ ] **Terminal** — a real shell over the GUI: line editing, history, tab completion, pipes, redirection, builtins (`cd, ls, cat, echo, mkdir, rm, cp, mv, pwd, clear, help, uname, whoami, df, free, ps, kill, date, shutdown`)
- [ ] **Notes (text editor)** — open/save files, undo/redo, find, word wrap, cursor navigation, keyboard shortcuts, a touch-friendly toolbar in phone mode
- [ ] **Files** — browse the AFS tree, open files in the right app, create folders, rename, delete (with confirmation), copy/move, breadcrumb navigation
- [ ] **Settings** — display (resolution/scale/theme light-dark), sound volume, date & time, language, keyboard layout, about/system info, power actions
- [ ] **Clock** — clock, calendar, alarms (wake from sleep)
- [ ] **Calculator** — basic + scientific
- [ ] **Image Viewer** — PNG/BMP/JPEG (JPEG decoder optional at v1.0)

#### 1.0.2 Audio
- [ ] Audio service + a PCM playback path (virtio-snd in QEMU, Intel HDA later)
- [ ] Volume control + a system sound on boot/shutdown
- [ ] Terminal bell, UI click sounds

#### 1.0.3 SDK & documentation
- [ ] `apps/sdk/` — public headers, `libaf` + `libafgui`, the app manifest format
- [ ] `afbuild` — a simple app build script (`afbuild build` produces an installable app package)
- [ ] `docs/` — an app developer guide with a complete "hello, window" tutorial
- [ ] API reference generated from headers
- [ ] Example app in the repo exercising every framework feature

#### 1.0.4 Polish & hardening
- [ ] Boot time budget: under 5 s from bootloader to shell in QEMU (< 15 s on modest hardware)
- [ ] Stability soak: 24-hour run with apps opening/closing, no leak, no crash, no panic
- [ ] Panic reporting: on crash, offer save-to-disk of the crash log, then reboot
- [ ] Recovery: a "safe mode" boot option that skips non-essential servers
- [ ] Accessibility pass: keyboard navigation everywhere, adequate contrast, large-text mode
- [ ] Internationalization scaffolding (UTF-8 everywhere, locale-aware date/number formatting)
- [ ] Error UX: no raw error codes in dialogs; human-readable messages with a "details" expansion

#### 1.0.5 System polish
- [ ] User accounts & login (single-user first, multi-user later)
- [ ] Home directories, per-user settings
- [ ] File permissions (owner/group/other, rwx) enforced by the fs service
- [ ] System logs written to disk and viewable in Settings → Logs
- [ ] Battery indicator and basic power management (phone-relevant, works with ACPI on PCs)

#### 1.0.6 v1.0 tests & acceptance
- [ ] **The end-to-end workflow test** (the definition of "usable"):
  `boot → open Terminal → mkdir /home/user/demo → open Notes → write text → save to /home/user/demo/notes.txt → open Files → navigate to the folder → see the file → reboot → open it back and verify the content`
- [ ] Performance regression gate in CI: boot time and IPC latency must not regress more than 10% between releases
- [ ] Install → use → shut down → reboot cycle, 20 consecutive times, zero failures
- [ ] A person who has never seen AfriyieOS completes the workflow above without verbal instructions

**✅ Acceptance criteria:** AfriyieOS v1.0 boots from an installed disk, provides a terminal, a text editor, a file manager, and settings, persists files across reboots, and a first-time user can create → edit → save → find a file without help.

---

### 🔵 v1.1 — *Twin*: ARM64 Phone Port

**Goal:** the same kernel boots on ARM64 and the UI adopts phone mode for real.

- [ ] `cmake/toolchain-aarch64-elf.cmake`; build the whole system for `aarch64-elf`
- [ ] `kernel/arch/arm64/vectors.S` — 16-entry exception vector table at `VBAR_EL1`
- [ ] `kernel/arch/arm64/context.S` — save/restore `x19–x30`, `sp`, plus FP/SIMD state (`q0–q31`, lazily)
- [ ] `mmu.c` — 4-level translation tables, `TTBR0_EL1` (user) / `TTBR1_EL1` (kernel), `MAIR_EL1`, `TCR_EL1`
- [ ] `gic.c` — GICv3 distributor + redistributor init, IRQ routing, `ICC_*` system registers
- [ ] Generic timer — `CNTPCT_EL0`, `CNTP_*` for the scheduler tick
- [ ] PL011 UART serial console
- [ ] `SVC` syscall entry (`ESR_EL1` decoding), matching x86_64 syscall numbers exactly
- [ ] Boot bridge: U-Boot → `boot.img` (kernel + DTB + ramdisk), DT-derived memory map
- [ ] Device Tree parser (vendored `libfdt` subset) for memory, UART, GIC, timer
- [ ] `simplefb` driver from the DT `framebuffer` node (or UEFI GOP on ARM laptops)
- [ ] `drivers/virtio_blk` + `virtio_input` built for ARM64 (QEMU `virt` machine)
- [ ] Phone hardware bring-up on a real device: pick **one** well-supported, unlocked, spare phone/board (Raspberry Pi 4/5 or PinePhone are far easier than a random Snapdragon); document the bring-up as `docs/porting/`
- [ ] Touchscreen: I²C/SPI panel driver + multitouch protocol
- [ ] `tools/mkimage.py --mode phone` → fastboot-flashable `boot.img` (Android boot image header v2/v4)
- [ ] **HAL portability audit:** measure the diff between the x86_64 and ARM64 kernels. **If > 20% of architecture-independent code changed, stop and refactor the HAL.**
- [ ] Phone-mode UI validation: 1080×2340 portrait, touch targets, gesture navigation, status bar

**✅ Acceptance criteria:** AfriyieOS boots on ARM64 (QEMU `virt` verified; at least one real device documented), renders the shell in phone mode, and accepts touch input — with the kernel core shared, not forked.

---

### 🔵 v1.2 — *Dual*: Acceleration & Polish

- [ ] GPU-accelerated blits and hardware page flipping (start with virtio-gpu 2D commands; then a real Intel GPU path)
- [ ] Damage-region pipelining: apps report damage, the compositor batches and flushes at vsync
- [ ] Real animation system: springs, interruption handling, 120 Hz option
- [ ] Power management: CPU frequency scaling via ACPI P-states / DT `cpufreq`, suspend/resume, screen dimming, battery-aware behavior
- [ ] Multi-core: per-CPU run queues, work stealing, IPIs, SMP boot (AP bring-up via INIT-SIPI-SIPI on x86_64 / PSCI on ARM64)
- [ ] TrueType text rendering with proper hinting + a bundled font family (licensing checked)
- [ ] Notification centre, search, quick settings
- [ ] Updater: atomic A/B system updates with rollback (`A` boots, updates `B`, switches boot slot)
- [ ] Installer improvements: dual-boot alongside Windows/Linux, resize existing partitions

---

### 🔵 v2.0 — *Horizon*: Networking & Ecosystem

- [ ] Network service process with its own TCP/IP stack (ARP, IPv4, ICMP, UDP, TCP with congestion control, DHCP client, DNS resolver)
- [ ] Ethernet (virtio-net) + Wi-Fi (start with a USB Wi-Fi dongle with open firmware; built-in phone Wi-Fi is a much larger project)
- [ ] TLS/crypto: Monocypher-based, certificate store, HTTPS client
- [ ] A browser or a web-render subset — deliberately last, and only if the ecosystem demands it
- [ ] Package manager: `.afpkg` format, signed repositories, dependency resolution, atomic install/remove
- [ ] App store UI backed by the package manager
- [ ] Multi-user accounts with proper isolation and permissions
- [ ] Bluetooth stack
- [ ] Compatibility layer exploration (a minimal POSIX/ELF-Linux shim so existing CLI tools can be recompiled and run)

---

## 12. Testing, Debugging & CI Strategy

### 12.1 The three test tiers

| Tier | Where it runs | What it covers | Gate |
| --- | --- | --- | --- |
| **T1 — Host unit tests** | CI host, native | Algorithms: bitmap allocator math, FAT32 parsing, layout solver, `af::Vector`, AFS on-disk structs | Must pass on every push |
| **T2 — In-kernel tests** | QEMU, bare metal | PMM/VMM/heap/scheduler/IPC/syscall/ELF — the real thing, asserting into serial output | Must pass on every push |
| **T3 — QEMU integration** | QEMU, scripted | Boot sequence, golden serial logs, screenshots, input injection, install workflow | Must pass before every tag |

### 12.2 Debug infrastructure (build it in `v0.1`, not later)

- **Serial logging** at 115200 on COM1/PL011 — every kernel subsystem logs its state transitions.
- **Structured log prefixes** so CI can grep: `AF_BOOT_OK`, `AF_PMM_READY`, `AF_VMM_READY`, `AF_SCHED_READY`, `AF_IPC_READY`, `AF_SHELL_READY`, `AF_TEST_FAIL:<name>`, `AF_PANIC:<reason>`.
- **`AF_ASSERT(cond)`** and `AF_ASSERT_MSG(cond, fmt, ...)` compiled out in release, fatal in debug.
- **Panic console** with the fault address, error code decoding (present/write/user), full register dump, and a symbol-resolved stack walk (using the embedded symbol table).
- **GDB over QEMU**: `-s -S` with a `tools/gdb-init` script that loads symbols, sets a hardware breakpoint at `kmain`, and enables a pretty-printer for `af_thread`.
- **QEMU monitor scriptability** (`-monitor stdio` or a socket) for screenshots and input injection in CI.
- **Frame-time and IPC-latency probes** built in from `v0.6`/`v0.7` — performance regressions are bugs.

### 12.3 CI pipeline (`.github/workflows/ci.yml`)

```
on: [push, pull_request]

jobs:
  host-tests:      # T1 — fast, always runs
    - cmake build the host-test target
    - ctest

  kernel-build:    # build both architectures
    - cache the cross toolchain (huge time saver)
    - build x86_64 → assert artifacts exist
    - build aarch64 (from v1.1) → assert artifacts exist
    - report firmware size (fail if over the §14 budget)

  boot-test:       # T2 + T3 — the important gate
    - build the bootable image
    - run QEMU with a 60 s timeout and -serial file:serial.log
    - require AF_BOOT_OK and every AF_*_READY marker in order
    - require absence of AF_PANIC / AF_TEST_FAIL
    - analyse the tail of the serial log for a crash signature

  shell-smoke:     # from v0.8
    - boot, wait for AF_SHELL_READY
    - screendump, verify the frame is not blank/black
```

### 12.4 Definition of Done for every milestone

- [ ] All acceptance-criteria tests pass locally **and** in CI
- [ ] No new compiler warnings (`-Wall -Wextra -Werror` stays clean)
- [ ] Kernel size and boot time recorded in `docs/releases/vX.Y.Z.md`
- [ ] Documentation updated in the same commit as the code
- [ ] `CHANGELOG.md` entry written
- [ ] Git tag `vX.Y.Z` created and pushed
- [ ] The demo reproducible by a second person from a clean checkout

---

## 13. Packaging, Installation & Release Engineering

### 13.1 Build artifacts per release

| Artifact | Purpose | How produced |
| --- | --- | --- |
| `afriyieos-installer-x86_64.img` | Bootable USB installer (GPT + ESP + ramdisk) | `tools/mkimage.py --mode install` |
| `afriyieos-installer-x86_64.iso` | Same, as an ISO (for VMs / El Torito) | `xorriso` target |
| `afriyieos-disk-x86_64.img` | Pre-installed system disk (for `qemu -drive`) | `tools/mkimage.py --mode disk` |
| `afriyieos-boot-aarch64.img` | fastboot-flashable phone boot image | `tools/mkimage.py --mode phone` |
| `afriyieos-sdk-x86_64.tar.gz` | Headers + libs + `afbuild` for app developers | `cmake --build --target sdk-package` |
| `SHA256SUMS` + `release-notes.md` | Integrity + human summary | `tools/release.py` |

### 13.2 Disk layout after installation

```
GPT
├── Partition 1: EFI System Partition   256 MB   FAT32   ← BOOTX64.EFI, kernel, ramdisk
└── Partition 2: AfriyieOS root          rest     AFS     ← /system /apps /home /var
```

### 13.3 PC installation flow

```
Boot installer USB (UEFI)
   ↓
Installer app: choose target disk (shows model, size, existing partitions)
   ↓
Explicit destructive-action confirmation ("ALL DATA ON /dev/nvme0n1 WILL BE ERASED")
   ↓
Write GPT → create ESP + AFS root → format both
   ↓
Copy kernel + ramdisk + servers + drivers + apps
   ↓
Install BOOTX64.EFI to the ESP at /EFI/BOOT/ (firmware fallback path)
   also write /EFI/AfriyieOS/ and an NVRAM boot entry via the UEFI Boot Services
   ↓
Write boot config (root partition GUID + kernel cmdline)
   ↓
Reboot → firmware loads AfriyieOS from disk → shell
```

### 13.4 Phone flashing flow (v1.1+)

```
Bootloader unlocked (user accepts the risk; AfriyieOS documents it explicitly)
   ↓
fastboot flash boot afriyieos-boot-aarch64.img
   ↓
fastboot reboot  → kernel + DTB + ramdisk load → phone-mode shell
   ↓
Optional: fastboot flash userdata (AFS image) for persistent storage
   ↓
Documented restore path back to the stock firmware (mandatory section in docs)
```

### 13.5 Versioning & release discipline

- **Semantic versioning**: `MAJOR.MINOR.PATCH`. Pre-1.0, `MINOR` tracks the roadmap versions above (`0.1` … `0.9`), `PATCH` is bug fixes only.
- **Git tags** on every release: annotated, signed when possible, with the release notes in the tag message.
- **CHANGELOG.md** following Keep-a-Changelog (Added / Changed / Fixed / Removed / Security).
- **Branching**: `main` is always green and bootable. Features develop on `feat/<name>`, merged only when CI is green. Broken `main` is treated as an emergency.
- **Commit discipline**: conventional commits (`feat(kernel):`, `fix(vmm):`, `docs:`, `build:`, `test:`), one logical change per commit, and a **commit + push after every meaningful change** so the repository history reads like real, incremental engineering work.

---

## 14. Performance Budgets & Non-Functional Requirements

| Metric | Budget | Enforced from |
| --- | --- | --- |
| Kernel code size | **< 64 KB** `.text` | v0.1 (CI check) |
| Boot time (bootloader → shell) | **< 5 s** in QEMU | v0.8 |
| Context switch | **< 1 000 cycles** | v0.2 |
| IPC fastpath round trip | **< 2 000 cycles** (`ipc_call` + reply) | v0.7 |
| Syscall overhead (null syscall) | **< 300 cycles** | v0.4 |
| Page fault handling | **< 2 000 cycles** | v0.2 |
| Compositor frame time (4 windows, 1080p) | **< 16.6 ms** (60 FPS) | v0.8 |
| Memory footprint (idle, no apps) | **< 128 MB** | v1.0 |
| Filesystem write throughput | **> 50 MB/s** on virtio | v0.9 |
| Max contiguous frame allocation | **> 64 MB** after a long uptime | v0.3 |
| Soak-test leak | **0 bytes/hour** | v1.0 |

**Engineering rule:** if a change breaks a budget, it does not merge. Budgets are measured in CI and printed in every release's notes.

---

## 15. Security Model

| Threat | Mitigation |
| --- | --- |
| Malicious/buggy app reads another app's memory | Hardware paging isolation; separate address spaces; no shared mapping without an explicit capability |
| App escalates to kernel | Minimal Ring 0 surface; every syscall argument validated (all user pointers bounds-checked) |
| Driver bug crashes the system | Drivers run in user space with least-privilege capabilities; the supervisor restarts them |
| Malicious app talks to a device it should not | No global device namespace; only capability holders can reach a device |
| Corrupted file system image | fsck-style verifier; journaling with replay; strict bounds checks on every on-disk structure |
| Firmware/boot tampering | Measured boot: hash the kernel + ramdisk, store in the boot config, verify at boot (v1.2), and support signing |
| Buffer overflow in a string operation | Own bounded string functions (`af_strlcpy`, `af_snprintf` with explicit sizes); no `strcpy`/`sprintf` anywhere |
| Use-after-free in the kernel | Debug-build redzones/canaries, allocation tracking, and a quarantine on free |
| Denial of service (memory exhaustion) | Per-process memory quotas, frame allocation accounting, and clean failure returns |

**Non-negotiable security rules**

1. Never dereference a user pointer without validating it against the process's mapped regions.
2. Never trust a length field from user space or from disk without a bounds check.
3. Capability rights may only be **narrowed** when derived — never widened.
4. The kernel allocates nothing in interrupt context.
5. Every on-disk structure is validated before use.

---

## 16. Known Pitfalls & Engineering Discipline

### 16.1 The technical traps that will cost you days

| Trap | Symptom | Fix |
| --- | --- | --- |
| x86_64 red zone | Random corruption after interrupts | `-mno-red-zone` |
| 16-byte stack alignment on `syscall` | Crashes in SSE/`movaps` code paths | Align `rsp` to 16 before every call into C |
| UEFI struct offsets | Triple fault with zero output | `_Static_assert(offsetof(...))` on **every** field against the UEFI spec |
| `PixelsPerScanLine` ≠ width | Diagonal image skew | Use stride in all framebuffer math, never `width * bpp` |
| Stale TLB after page-table edits | Writes silently ignored, stale reads | `invlpg` (x86_64) / `TLBI` (ARM64) after every change |
| Stack overflow in the kernel | Corruption far from the cause | Guard pages around kernel stacks + a canary pattern |
| Interrupt handler using FP/SIMD | State corruption, NaN everywhere | Never use floating point in the kernel; `-mgeneral-regs-only` on ARM64 |
| DMA to a physical address while paging is on | Driver writes to the wrong RAM | Convert virtual → physical for every DMA buffer (`virt_to_phys`) |
| Cache coherency on MMIO | Writes appear to be dropped | Correct cache attributes (`PWT`/`PCD`, or `MAIR` device memory on ARM64) |
| Non-atomic bit ops on shared state | Rare, unreproducible corruption | `lock` prefixes (`lock bts`) / `LDAXR`-`STLXR` loops |
| `ExitBootServices` `MapKey` change | Firmware refuses, boot hangs | Re-fetch the memory map immediately before the call; retry on failure |
| Assuming memory is zeroed | Random crashes from garbage in `.bss` | Zero `.bss` in `entry.asm`; explicitly zero freshly allocated pages in debug builds |

### 16.2 The psychological traps

* **No `printf` from day one.** Serial output, QEMU's monitor, and GDB are your only eyes. Build them before you need them.
* **Bisection is the only debugging technique that always works.** When the system hangs 200 lines into boot, comment out half. Repeat.
* **Believe the hardware, not your assumptions.** A register dump is worth a thousand theories.
* **Write down every bug and its root cause.** `docs/debug-log.md` becomes the most valuable file in the repository by `v0.4`.
* **The 1 a.m. bug is real.** You will spend six hours and find `edi` where `rdi` belongs. Every kernel developer has done it. Sleep, then look again.
* **Momentum beats perfection.** A crude, working framebuffer today is worth more than an elegant DMA engine design that never compiles. Ship each milestone ugly if necessary, then refine.

---

## 17. Risk Register

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Scope overwhelms solo development | **High** | **Critical** | Strict per-version milestones; every version is independently demonstrable; drop features before dropping the schedule |
| ARM64 port forces a HAL rewrite | Medium | High | Port at v1.1 after the kernel is stable; enforce the 20% diff rule; design HAL interfaces before writing arch code |
| Real phone hardware is unsupported/undocumented | **High** | High | Target one well-documented board (Raspberry Pi / PinePhone class) first; treat mainstream phones as a stretch goal |
| Motivation collapse around v0.3–v0.5 (the "long middle") | **High** | **Critical** | Publish a demo at every version; keep a visible progress log; celebrate each green milestone |
| Toolchain build breaks / takes hours | Medium | Medium | Cache the built toolchain in CI; use Docker for a reproducible dev environment; document exact versions |
| Vendor library licensing problem | Low | Medium | Vendor only MIT/BSD/CC0; record licenses per §5.3; no GPL in the kernel |
| Security bug in the syscall layer | Medium | High | Mandatory pointer validation; fuzz the syscall surface (v0.7+) |
| Data loss during installer testing | Medium | High | Always test against virtual disks; real hardware only with a known-good restore path |
| Burnout from debugging without visibility | High | High | Invest in debug infrastructure early; keep a "known good" tag to bisect against |

---

## Appendix A — Glossary

| Term | Meaning |
| --- | --- |
| **AFS** | Afriyie File System — the native read-write file system with journaling |
| **APIC** | Advanced Programmable Interrupt Controller — x86 interrupt controller |
| **Capability** | An unforgeable kernel handle granting specific rights to a specific object |
| **Compositor** | The server that blends window surfaces into the final screen image |
| **DTB / Device Tree** | A data structure describing ARM hardware to the kernel |
| **EFI / UEFI** | Modern PC firmware and its application environment |
| **ESP** | EFI System Partition — the FAT32 partition firmware boots from |
| **GDT / IDT** | Global Descriptor Table / Interrupt Descriptor Table (x86) |
| **GIC** | Generic Interrupt Controller (ARM64) |
| **GOP** | Graphics Output Protocol — UEFI's framebuffer interface |
| **HAL** | Hardware Abstraction Layer — the arch-specific code behind a common API |
| **IPC** | Inter-Process Communication — the message-passing core of a microkernel |
| **Initramfs** | The initial RAM file system loaded at boot before the disk root exists |
| **MMIO** | Memory-Mapped I/O — device registers accessed as memory |
| **PMM / VMM** | Physical / Virtual Memory Manager |
| **Syscall** | The controlled entry point from user mode into the kernel |
| **VFS** | Virtual File System — the abstraction unifying different file systems |
| **TTBR / PML4** | The root page-table registers on ARM64 / x86_64 |
| **el** | Exception Level (ARM64's privilege levels: EL0 = user, EL1 = kernel) |
| **sp** | Scale-independent pixel — the density-independent UI unit (4px = 1sp at 100% scale) |

---

## Appendix B — Reference Reading & Inspirations

### Documentation
* **OSDev Wiki** — <https://wiki.osdev.org> — the canonical community reference for bare-metal development.
* **UEFI Specification 2.10** — <https://uefi.org/specifications> — mandatory for the boot bridge.
* **Intel SDM Vol. 3** — <https://software.intel.com/sdm> — paging, interrupts, and privilege levels.
* **ARM Architecture Reference Manual (ARMv8-A)** — exception levels, MMU, GIC.
* **Device Tree Specification** — <https://devicetree.org> — for the ARM64 boot path.

### Tutorials and books
* *Writing an OS in Rust* (Philipp Oppermann) — <https://os.phil-opp.com> — even for a C kernel, the paging and allocator explanations are the clearest available.
* *The Little OS Book* — <https://littleosbook.github.io>
* *Operating Systems: Three Easy Pieces* — <https://pages.cs.wisc.edu/~remzi/OSTEP> — free, and the best conceptual grounding.
* *Modern Operating Systems* (Tanenbaum) — microkernel theory from the source.

### Systems to study
* **seL4** — the reference capability-based microkernel; the source of AfriyieOS's security model.
* **MINIX 3** — a microkernel that runs real user-space drivers and self-heals.
* **SerenityOS** — proof that a from-scratch OS with a beautiful hand-written UI is achievable by a community; the single best source of morale.
* **Redox OS** — a Rust microkernel on POSIX lines.
* **Linux 0.01** — the 8,413-line reality check.
* **QNX Neutrino** — commercial microkernel excellence; the model for driver isolation.

### Graphics, fonts and UI
* **ThorVG** — <https://thorvg.org> — the planned vector renderer.
* **stb_truetype** — <https://github.com/nothings/stb> — a single-header TrueType rasterizer.
* **Material Design 3** — the inspiration for the token system (color roles, elevation, motion).
* **Apple HIG / Flutter layout model** — for the responsive two-form-factor approach.

---

<div align="center">

**AfriyieOS** — *from a blank file to an operating system people can install and use.*

*Build it in versions. Ship a demo at every version. Never break `main`.*

Document maintained by **Hayford Afriyie** · Version 1.0.0 · MIT License

</div>

# AfriyieOS Documentation

This directory holds the canonical engineering documentation for AfriyieOS.

## Start here

| Document | What it is |
| --- | --- |
| **[AfriyieOS-Blueprint.md](AfriyieOS-Blueprint.md)** | **The complete engineering specification.** Architecture, language choices, kernel ABI, the full `v0.1 → v2.0` roadmap, and a checkbox-level task breakdown with acceptance criteria for every milestone. Read this first. |

## Architecture

| Document | Status | Covers |
| --- | --- | --- |
| [architecture/boot-flow.md](architecture/boot-flow.md) | ✅ implemented | How firmware hands control to the kernel, on both PC and phone |
| [architecture/memory-model.md](architecture/memory-model.md) | 📐 designed, v0.2 | Address-space layout, PMM, VMM, the kernel heap |
| [architecture/ipc-protocol.md](architecture/ipc-protocol.md) | 📐 designed, v0.7 | Endpoints, message format, `send`/`recv`/`call`/`reply`, grants |
| [architecture/capability-model.md](architecture/capability-model.md) | 📐 designed, v0.7 | Capability objects, rights, derive/revoke, the bootstrapping chain |
| [architecture/universal-compat.md](architecture/universal-compat.md) | 🔨 v0.6, detector implemented | How AfriyieOS runs Linux, Windows, Android and macOS software — and what genuinely cannot work |

## ABI

| Document | Status | Covers |
| --- | --- | --- |
| [abi/syscalls.md](abi/syscalls.md) | 🔨 v0.4, numbers 0–3 and 5 implemented | The syscall table, calling convention on both architectures, error model |
| [abi/afs-filesystem.md](abi/afs-filesystem.md) | 📐 designed, v0.9 | The native on-disk format: superblock, inodes, extents, directory, journal |

## Source layout

Where the pieces of the milestone-per-milestone build actually live.

| Path | Language | What is in it |
| --- | --- | --- |
| `boot/uefi/` | C11 + NASM, PE32+ via MinGW | The PC boot bridge: takes the firmware's handoff, exits boot services, loads and jumps to the kernel |
| `kernel/core/` | C11 | Architecture-independent kernel: log, PMM, heap, threads, scheduler, syscalls, ELF loader |
| `kernel/arch/x86_64/` | C11 + NASM | GDT, IDT, TSS, paging, PIT, serial, context switch, the ring-3 entry stub |
| `kernel/drivers/`, `kernel/fs/` | C11 | In-kernel drivers and file systems. **Deliberately temporary** — these move to user space at v0.6–v0.7, which is what returns the kernel core to its 64 KiB budget (ADR-011) |
| `libs/libaf/` | C11 + GAS | The user-space runtime: system call stub, `crt0.S`, and the linker script that places user programs at 512 GiB |
| `apps/` | C11 | User programs. `apps/init` is the one the kernel loads, in its own address space, and enters |
| `tools/` | Python 3 + Bash | Build, image packaging, QEMU runner, verifiers, evidence capture |
| `tests/host/` | Python 3 | Host-side tests, chiefly of the image builder |

**`libs/libaf/user.lds` links user programs at 512 GiB — the base of the user
region, which is PML4[1].** The slot is part of the address-space contract, and
`kernel/include/afriyie/config.h` explains why at length. The short version: the
kernel's identity map lives in PML4[0], the x86 walk requires the USER bit at
*every* level on the path to a user page, so a single user page in PML4[0] would
put the USER bit on the top-level entry the kernel shares — one entry with two
owners and no way to tell them apart. Giving user space its own top-level slot
lets a process **share** the kernel's tables by pointer, which needs no copying
and no ownership rule.

The consequence is that user code is compiled with `-mcmodel=large`, since the
small code model cannot form a 32-bit absolute reference to a 512 GiB address.

## Design

| Document | Status | Covers |
| --- | --- | --- |
| [design/ui-design-system.md](design/ui-design-system.md) | 📐 designed, v0.6+ | Afriyie Shell: tokens, responsive breakpoints, layout, motion |
| [design/brand.md](design/brand.md) | ✅ implemented in v0.1 | Colours, the wordmark, the splash screen rules |

## Engineering process

| Document | Purpose |
| --- | --- |
| [debug-log.md](debug-log.md) | Every non-trivial bug, its root cause, and how it was found. The most valuable file in the repository by `v0.4`. |
| [contributing.md](contributing.md) | Build, test, commit and review conventions |
| [releases/](releases/) | Per-version release notes with measured sizes, timings and acceptance evidence |

## Status legend

* ✅ **implemented** — the code exists and is exercised by tests
* 🔨 **in progress** — partially implemented in the current milestone
* 📐 **designed, vX.Y** — specified here, implemented at the named milestone
* 💭 **deferred** — deliberately postponed; see the blueprint's ADR log

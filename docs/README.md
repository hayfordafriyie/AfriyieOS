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

## ABI

| Document | Status | Covers |
| --- | --- | --- |
| [abi/syscalls.md](abi/syscalls.md) | ✅ v0.4 surface frozen | The syscall table, calling convention on both architectures, error model |
| [abi/afs-filesystem.md](abi/afs-filesystem.md) | 📐 designed, v0.9 | The native on-disk format: superblock, inodes, extents, directory, journal |

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

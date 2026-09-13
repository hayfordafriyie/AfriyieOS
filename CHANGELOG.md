# Changelog

All notable changes to AfriyieOS are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Pre-1.0, `MINOR` versions track the roadmap milestones defined in
[docs/AfriyieOS-Blueprint.md](docs/AfriyieOS-Blueprint.md).

---

## [Unreleased]

### Added

- **Complete engineering blueprint** — `docs/AfriyieOS-Blueprint.md`
  - Layered system architecture for PCs (x86_64/UEFI) and phones (ARM64/U-Boot)
  - Capability-based microkernel design (seL4-inspired isolation model)
  - Language and framework decisions with rationale (C11 · C++20 · NASM/GAS · ThorVG)
  - Kernel ABI specification: syscall table, IPC message format, capability rights,
    service protocol label namespaces
  - Full `v0.1 → v2.0` roadmap with per-milestone task breakdowns and acceptance criteria
  - Performance budgets, security model, risk register and debugging strategy
- Repository scaffolding: `README.md`, `LICENSE` (MIT), `.gitignore`, `CHANGELOG.md`

---

## Milestone index

| Version | Codename | Theme | Status |
| --- | --- | --- | --- |
| v0.1 | Seed | Boot & display | ⏳ planned |
| v0.2 | Roots | Memory & multitasking | ⏳ planned |
| v0.3 | Trunk | Disk & file system | ⏳ planned |
| v0.4 | Branches | User mode & syscalls | ⏳ planned |
| v0.5 | Leaves | Drivers & input | ⏳ planned |
| v0.6 | Bloom | Graphics core | ⏳ planned |
| v0.7 | Fruit | IPC & user-space services | ⏳ planned |
| v0.8 | Canopy | Window manager & shell | ⏳ planned |
| v0.9 | Harvest | Installer & images | ⏳ planned |
| v1.0 | Oseadeɛyɔ | Installable & usable | ⏳ planned |
| v1.1 | Twin | ARM64 phone port | ⏳ planned |
| v1.2 | Dual | Acceleration & polish | ⏳ planned |
| v2.0 | Horizon | Networking & ecosystem | ⏳ planned |

---

[Unreleased]: https://github.com/hayfordafriyie/AfriyieOS/commits/main

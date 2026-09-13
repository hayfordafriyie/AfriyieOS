# Memory Model

**Status:** 📐 designed · 📐 implemented at v0.2

For v0.1 the kernel is identity-mapped at 1 MiB with no paging: the framebuffer,
the boot structure and the kernel are all reached at their physical addresses.
This document specifies the final model and exactly what changes.

---

## 1. Address space layout

Identical on both architectures. This is the single most important decision for
portability, because it means page-table code differs but everything that *uses*
an address does not.

```
0x0000_0000_0000_0000 ┌──────────────────────────────┐
                      │ unmapped guard page          │  catch null dereferences
0x0000_0000_0000_1000 ├──────────────────────────────┤
                      │ USER TEXT       (r-x)        │ \
                      │ USER RODATA     (r--)        │  | per-process
                      │ USER DATA       (rw-)        │  | user address space
                      │ USER BSS        (rw-)        │ /
                      ├──────────────────────────────┤
                      │ USER HEAP       ↓ grows down │
                      │        (unmapped gap)        │
                      │ USER STACK      ↑ grows down │
0x0000_7FFF_FFFF_F000 ├──────────────────────────────┤
                      │ NON-CANONICAL / unmapped gap │
0xFFFF_8000_0000_0000 ├──────────────────────────────┤
                      │ PHYSICAL DIRECT MAP          │  all RAM, offset-mapped
                      │                              │  (2 MiB huge pages)
0xFFFF_C000_0000_0000 ├──────────────────────────────┤
                      │ KERNEL IMAGE + KERNEL HEAP   │  higher half
0xFFFF_FFFF_8000_0000 ├──────────────────────────────┤
                      │ KERNEL STACKS + IST          │
0xFFFF_FFFF_FFFF_FFFF └──────────────────────────────┘
```

### 1.1 Why a direct map at all

Device drivers and the PMM need to touch arbitrary physical memory — DMA
buffers, page tables, a page that is not currently mapped. Walking page tables to
reach every such address is slow and, worse, impossible when the page-table code
itself is what needs the address.

The direct map solves this: a fixed linear translation

```
virtual = AF_DIRECT_MAP_BASE + physical
```

so any physical address is reachable at a known virtual address with no lookup.
`AF_DIRECT_MAP_BASE` is `0xFFFF_8000_0000_0000`, and the kernel keeps the two
conversions as functions rather than arithmetic spread through the code:

```c
af_vaddr hal_phys_to_virt(af_paddr pa);   // AF_DIRECT_MAP_BASE + pa
af_paddr hal_virt_to_phys(af_vaddr va);   // va - AF_DIRECT_MAP_BASE
```

**Every DMA buffer address must go through `hal_virt_to_phys`.** Handing a kernel
virtual address to a device that performs physical DMA writes to unrelated RAM,
and the resulting corruption is far from the cause.

### 1.2 Why the gap is real

The non-canonical gap is not cosmetic. On x86_64, bits 63..48 must all equal bit
47 for an address to be valid; anything in between raises `#GP`. On ARM64 the
equivalent split is `TTBR1` for addresses with the top byte set and `TTBR0` for
those without. Leaving the middle unmapped means a wild pointer is a clean fault
rather than a silent alias of something important.

---

## 2. Architectural differences the HAL absorbs

| Concern | x86_64 | ARM64 |
| --- | --- | --- |
| Table root | `PML4`, loaded into `CR3` | `TTBR0_EL1` (user) / `TTBR1_EL1` (kernel) |
| Levels | 4: PML4 → PDPT → PD → PT | 4: L0 → L1 → L2 → L3 |
| Entries per table | 512 | 512 |
| Huge pages | 2 MiB (PD), 1 GiB (PDPT) | 2 MiB (L2), 1 GiB (L1) |
| Address-space switch | Write `CR3` | Write `TTBR0_EL1` + `ISB` |
| TLB invalidation | `invlpg` / `CR3` reload | `TLBI VAE1` / `TLBI VMALLE1` + `DSB` + `ISB` |
| Cache attributes | PAT bits 6 (PCD) and 5 (PWT) | `MAIR_EL1` indices |
| Execute control | NX bit (bit 63) | PXN/UXN bits (53/54) |
| Access flag | Not present | `AF` bit (10) — must be set or the first access faults |
| Table alignment | 4 KiB | 4 KiB |

Both are 4-level, 512-entry, 4 KiB-granular. The differences are encodings and
maintenance, not structure — which is exactly why one `hal_map_page` interface
can serve both.

---

## 3. Physical Memory Manager (v0.2)

### 3.1 Input

The PMM's entire view of the machine is `boot_info->memory_regions`, already
normalised by the boot bridge (§ boot-flow). It knows nothing about UEFI or
Device Trees.

### 3.2 Representation

A **bitmap**: one bit per 4 KiB frame. 1 means allocated, 0 means free.

```
4 GiB of RAM  →  1 048 576 frames  →  128 KiB of bitmap
64 GiB of RAM →  16 777 216 frames →  2 MiB of bitmap
```

2 MiB is an acceptable price for a structure with O(1) allocation, no
fragmentation of the metadata itself, and a trivial debug story (the popcount is
the number of used frames).

### 3.3 Initialisation

1. `af_boot_info_max_address()` gives the highest address, which sizes the
   bitmap.
2. Every frame is marked **used** — the default must be the safe one, so a bug in
   the region loop leaks memory rather than handing out firmware.
3. Every `AF_MEM_USABLE` and `AF_MEM_BOOTLOADER` region is marked **free**.
   `AF_MEM_ACPI_RECLAIM` becomes free once the tables are parsed at v0.5.
4. Then explicitly re-mark as used: the kernel image, the bitmap itself, and —
   critically — the **boot structure at `0x7000`**, which is not inside any
   region the firmware knows about because the boot bridge put it in low memory.
   Forgetting this is a rare, agonising corruption bug.
5. Never allocatable: `AF_MEM_RESERVED`, `AF_MEM_BAD`, `AF_MEM_ACPI_NVS`,
   `AF_MEM_RUNTIME`, `AF_MEM_TABLE`, `AF_MEM_FRAMEBUFFER`.

### 3.4 Interface

```c
af_paddr pmm_alloc_frame(void);                 // one 4 KiB frame, zeroed in debug
af_paddr pmm_alloc_frames(af_u32 count);        // contiguous run, or 0
void     pmm_free_frame(af_paddr frame);
void     pmm_free_frames(af_paddr base, af_u32 count);
af_u32   pmm_frame_refcount(af_paddr frame);    // for CoW and shared memory
void     pmm_frame_ref(af_paddr frame);
void     pmm_frame_unref(af_paddr frame);
```

Returns `0` on failure. Physical address 0 is never a valid frame, so it is a safe
sentinel — and the boot bridge marks the first page reserved for exactly this
reason.

### 3.5 Refcounting

Reference counting is not an optimisation; it is required by two later features:

* **Copy-on-write fork** (v0.4) — two address spaces share a read-only frame.
* **Memory grants** (v0.7) — a shared buffer is mapped into a second process.

Both need "free this frame only when the last user is gone". Retrofitting
refcounts after the allocator has users is a large, error-prone change, so the
count exists from the start even though v0.2 has exactly one user per frame.

### 3.6 Statistics

`pmm_stats_t`: total, free and used frames, and the largest contiguous free run.
A falling largest-run figure under a stable allocation count is the earliest
signal of fragmentation, and it costs nothing to track.

---

## 4. Virtual Memory Manager (v0.2)

### 4.1 Responsibility split

The VMM owns *address spaces and policy*. The HAL owns *encodings*.

```
      vmm_map(space, va, pa, VMM_WRITABLE | VMM_USER)
                          │
                          │  translate portable flags → arch bits,
                          │  allocate intermediate tables
                          ▼
                hal_map_page(space->root, va, pa, HAL_PRESENT | HAL_WRITABLE | HAL_USER)
```

The VMM never sees a PML4 entry; the HAL never sees a `struct af_address_space`.

### 4.2 Portable flags

```c
#define VMM_READ     (1u << 0)
#define VMM_WRITE    (1u << 1)
#define VMM_EXEC     (1u << 2)
#define VMM_USER     (1u << 3)
#define VMM_GLOBAL   (1u << 4)   // not flushed on address-space switch
#define VMM_NOCACHE  (1u << 5)
#define VMM_DEVICE   (1u << 6)   // MMIO: no caching, no speculation
#define VMM_HUGE     (1u << 7)   // 2 MiB page
```

`VMM_DEVICE` matters more than it looks: mapping a device register range as
normal cacheable memory produces dropped writes and reads that appear to return
stale values, with no fault to point at the cause.

### 4.3 Interface

```c
af_status_t vmm_map(af_address_space_t *space, af_vaddr va, af_paddr pa, af_u32 flags);
af_status_t vmm_map_range(af_address_space_t *space, af_vaddr va, af_paddr pa,
                          af_u64 size, af_u32 flags);
af_status_t vmm_unmap(af_address_space_t *space, af_vaddr va);
af_status_t vmm_unmap_range(af_address_space_t *space, af_vaddr va, af_u64 size);
af_status_t vmm_protect(af_address_space_t *space, af_vaddr va, af_u64 size, af_u32 flags);
af_paddr    vmm_translate(af_address_space_t *space, af_vaddr va);
af_u32      vmm_query(af_address_space_t *space, af_vaddr va);   // 0 if unmapped

af_address_space_t *vmm_new_address_space(void);
void                vmm_destroy_address_space(af_address_space_t *space);
```

### 4.4 TLB maintenance is not optional

Every mapping change must be followed by invalidation, or the CPU may keep using
a stale translation indefinitely. The symptom is writes that appear to be ignored
and reads that return old data — with no fault.

| Operation | x86_64 | ARM64 |
| --- | --- | --- |
| One page | `invlpg` | `TLBI VAE1` + `DSB` + `ISB` |
| Whole space | reload `CR3` | `TLBI VMALLE1` + `DSB` + `ISB` |

`af_flush_tlb_page()` and `af_flush_tlb_all()` in `io.h` already provide both, so
there is one place to get this right.

### 4.5 Huge pages

The direct map covers all RAM with 2 MiB pages. This is not an optimisation
detail — mapping 64 GiB with 4 KiB pages needs 16 million page-table entries,
about 128 MiB of tables just to describe the memory that holds the tables.

### 4.6 The `#PF` handler

At v0.2 a page fault is always fatal and prints a full diagnostic: the faulting
address from `CR2`, and the error code decoded into present/protection,
read/write, user/kernel and instruction-fetch. At v0.4 the same handler grows two
recovery paths — demand paging and copy-on-write — and the v0.2 diagnostic is
what makes those debuggable.

---

## 5. Kernel heap (v0.2)

### 5.1 Slab allocator

One cache per common size class: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.
Each cache draws whole pages from the PMM and slices them into same-sized
objects, with a free list threaded through the free objects themselves.

Requests larger than 2048 bytes go straight to `pmm_alloc_frames()` as a
contiguous run.

```c
void *kmalloc(af_size size);
void *kzalloc(af_size size);
void *krealloc(void *ptr, af_size new_size);
void  kfree(void *ptr);
void *kmalloc_aligned(af_size size, af_size alignment);
```

### 5.2 Why slabs and not a free-list heap

* **No fragmentation within a size class.** A classic heap degrades under
  mixed-size churn; slabs cannot.
* **O(1) allocate and free.**
* **Debug hooks are cheap.** With one cache per size, a redzone check on every
  free verifies every allocation of that size.

### 5.3 Debug mode

In a debug build:

* a **redzone** of `0xAF` bytes before and after every allocation, verified on
  free — this catches the most common kernel bug class;
* every live allocation recorded with file and line, so a leak report says where
  the memory came from;
* freed memory poisoned with `0xFD`, so a use-after-free reads obviously wrong
  data rather than plausible data;
* freshly allocated frames and objects zeroed, so "uninitialised" bugs surface at
  v0.2 rather than at v0.9.

All of it compiles out of a release build.

---

## 6. Address-space lifecycle

```
vmm_new_address_space()
    │  allocate a root table
    │  copy the kernel half of the current root  (shared, never per-process)
    │  leave the user half empty
    ▼
af_address_space_t  ──►  vmm_map / vmm_map_range / vmm_protect
    │
    │  at v0.4: ELF loader maps PT_LOAD segments here,
    │           a stack is mapped with a guard page below it
    ▼
process runs
    │
    │  v0.4: fork() shares frames read-only and takes #PF on write
    ▼
vmm_destroy_address_space()
       unmap every user range, unref every frame,
       free every intermediate table, free the root
```

Kernel mappings are **shared, not copied**: every address space points at the
same kernel page tables. Copying them would waste hundreds of megabytes and
guarantee they drift apart.

Guard pages sit immediately below every kernel stack and every user stack. A
stack overflow then faults at a clean, diagnosable address instead of silently
corrupting whatever is adjacent.

---

## 7. The higher-half move (v0.2)

What changes, all in one commit because they are not separable:

1. `kernel/linker/x86_64.lds` gains a higher-half VMA
   (`0xFFFFFFFF80000000`) with a physical load address of 1 MiB, so VMA ≠ LMA.
2. `AF_KERNEL_HIGHER_HALF=ON`, which adds `-mcmodel=kernel`.
3. A short assembly bootstrap builds initial page tables mapping the higher half
   onto the physical kernel **and** installing the direct map, loads `CR3`, and
   jumps to the higher-half entry.
4. `hal_phys_to_virt` and `hal_virt_to_phys` start returning real answers.
5. `af_boot_info` is copied out of low memory into the kernel heap before the
   low mapping is discarded.

The failure mode of getting any one of these wrong is an instant triple fault
with no output. That is why they land together, and why the v0.1 kernel ships
identity-mapped rather than half-converted.

---

## 8. Budgets

From blueprint §14:

| Metric | Budget |
| --- | --- |
| Context switch | < 1 000 cycles |
| Page fault handling | < 2 000 cycles |
| Max contiguous allocation after a long uptime | > 64 MiB |
| Memory leak under a 24-hour soak | 0 bytes/hour |
| Idle footprint with no applications | < 128 MiB |

---

## 9. References

* Intel SDM Vol. 3A, chapter 4 — paging
* ARM ARM (ARMv8-A), chapter D5 — the MMU
* [boot-flow.md](boot-flow.md) — how the memory map is obtained
* [capability-model.md](capability-model.md) — how `MEMORY_FRAME` becomes a right
* Writing an OS in Rust, "Paging" and "Heap Allocation" — the clearest published
  walkthroughs of these structures, even for a C kernel

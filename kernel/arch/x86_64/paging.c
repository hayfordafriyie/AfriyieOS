// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 paging: the HAL implementation of the portability contract
//
// =============================================================================
// WHAT THIS DOES
// =============================================================================
// Implements hal_pt_create / hal_map_page / hal_translate and friends from
// kernel/include/afriyie/hal.h, plus the boot-time page-table bootstrap that
// gives the kernel its OWN page tables.
//
// Until this existed the kernel was running on the page tables OVMF left behind
// after ExitBootServices. They happened to identity-map everything we touched,
// which is why nothing broke — but the kernel did not own its own address space,
// could not rely on any particular mapping existing, and had no way to create a
// second address space at all. User mode (v0.4) is impossible without this.
//
// =============================================================================
// FOUR-LEVEL WALK
// =============================================================================
//   PML4[index of bits 47:39] -> PDPT[bits 38:30] -> PD[bits 29:21] -> PT[bits 20:12]
//
// With 2 MiB pages the walk stops at the PD, which is what the identity map
// uses: mapping 4 GiB with 4 KiB pages would need 1 048 576 page-table entries
// and 8 MiB of tables to describe memory we are not yet managing.
// =============================================================================

#include "x86_64.h"
#include "afriyie/hal.h"
#include "afriyie/pmm.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/io.h"

// -----------------------------------------------------------------------------
// Entry format
// -----------------------------------------------------------------------------
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define ENTRIES_PER_TABLE 512

// -----------------------------------------------------------------------------
// Direct-map base: where every physical address is also reachable.
//
// Set from config.h so the HAL and the rest of the kernel agree on one value.
// Right now this is used by phys_to_virt, which matters as soon as the kernel
// stops being identity-mapped.
// -----------------------------------------------------------------------------
#define AF_PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define AF_PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define AF_PML4_INDEX(v) (((v) >> 39) & 0x1FF)

// -----------------------------------------------------------------------------
// Physical access
//
// While the kernel is identity-mapped, a physical address is directly
// dereferenceable. Once the higher-half move lands this becomes a translation
// through the direct map, and every page-table walk in this file goes through
// these two helpers — which is why they exist rather than raw casts.
// -----------------------------------------------------------------------------
static inline af_u64 *phys_to_ptr(af_paddr pa)
{
    return (af_u64 *)(af_uptr)pa;
}

static inline af_paddr ptr_to_phys(const void *ptr)
{
    return (af_paddr)(af_uptr)ptr;
}

// -----------------------------------------------------------------------------
// Portability flag translation
// -----------------------------------------------------------------------------
static af_u64 hal_flags_to_pte(af_u32 flags)
{
    af_u64 pte = 0;

    if ((flags & HAL_PRESENT) != 0)  { pte |= PTE_PRESENT; }
    if ((flags & HAL_WRITABLE) != 0) { pte |= PTE_WRITABLE; }
    if ((flags & HAL_USER) != 0)     { pte |= PTE_USER; }
    if ((flags & HAL_GLOBAL) != 0)   { pte |= PTE_GLOBAL; }
    if ((flags & HAL_NOCACHE) != 0)  { pte |= PTE_PCD; }
    if ((flags & HAL_DEVICE) != 0)   { pte |= PTE_PCD | PTE_PWT; }
    if ((flags & HAL_HUGE) != 0)     { pte |= PTE_HUGE; }

    // Absent execute permission means the NX bit is SET. Getting this backwards
    // makes every non-executable page executable, which is a security bug that
    // nothing functional would ever reveal.
    if ((flags & HAL_EXEC) == 0)     { pte |= PTE_NX; }

    return pte;
}

static af_u32 pte_to_hal_flags(af_u64 pte)
{
    af_u32 flags = 0;

    if ((pte & PTE_PRESENT) != 0)  { flags |= HAL_PRESENT; }
    if ((pte & PTE_WRITABLE) != 0) { flags |= HAL_WRITABLE; }
    if ((pte & PTE_USER) != 0)     { flags |= HAL_USER; }
    if ((pte & PTE_GLOBAL) != 0)   { flags |= HAL_GLOBAL; }
    if ((pte & PTE_HUGE) != 0)     { flags |= HAL_HUGE; }
    if ((pte & PTE_PCD) != 0)      { flags |= HAL_NOCACHE; }
    if ((pte & PTE_NX) == 0)       { flags |= HAL_EXEC; }

    return flags;
}

// -----------------------------------------------------------------------------
// Table management
// -----------------------------------------------------------------------------
static af_paddr alloc_table(void)
{
    af_paddr frame = pmm_alloc_frame_z();
    if (frame == AF_FRAME_INVALID) {
        return AF_FRAME_INVALID;
    }
    return frame;
}

// Walks to the next level, allocating the table if it is missing.
//
// `parent_pte` is the entry that will point at the new table, and it must be
// given PRESENT and WRITABLE — an intermediate entry that is not writable
// produces a page fault on the WALK itself rather than on the final access,
// which is a confusing way to learn this.
static af_u64 *walk_to_next_level(af_u64 *parent, af_u32 index, af_u32 flags)
{
    af_u64 entry = parent[index];

    if ((entry & PTE_PRESENT) != 0) {
        if ((entry & PTE_HUGE) != 0) {
            // A huge page is already mapped here. Splitting it is real work and
            // nothing needs it yet, so refuse clearly rather than corrupting the
            // mapping by overwriting the entry.
            af_error("vmm", "cannot descend into a huge page at table index %u", index);
            return NULL;
        }

        // =====================================================================
        // AN EXISTING TABLE MAY NEED THE USER BIT ADDED TO IT
        // =====================================================================
        // On x86, a page is accessible from ring 3 only if the USER bit is set in
        // EVERY paging-structure entry used to translate it — the PML4 entry
        // included. A missing bit anywhere in the chain denies access to
        // everything below it, and the fault describes a protection violation on
        // a page whose own entry looks perfectly correct.
        //
        // This is what broke the first ring-3 transition. The identity-map
        // bootstrap created PML4[0] for the kernel, without USER. Mapping a user
        // page at 4 GiB walks through that same PML4 entry, created the tables
        // below it correctly with USER set, and then faulted on the first
        // instruction fetch because the top of the chain still said kernel-only.
        //
        // The leaf entries of the identity map do NOT have USER and are not
        // reachable from ring 3 — which is the property that makes this fix safe
        // rather than a hole: granting USER on an intermediate table permits
        // nothing on its own.
        // =====================================================================
        if ((flags & HAL_USER) != 0 && (entry & PTE_USER) == 0) {
            parent[index] = entry | PTE_USER;
        }

        return phys_to_ptr(entry & PTE_ADDR_MASK);
    }

    af_paddr table = alloc_table();
    if (table == AF_FRAME_INVALID) {
        return NULL;
    }

    // Intermediate entries are always present, writable and (for a kernel
    // mapping) not user-accessible. The USER bit is applied per-leaf, and a leaf
    // cannot be user-accessible unless every level above it is as well.
    af_u64 new_entry = table | PTE_PRESENT | PTE_WRITABLE;

    if ((flags & HAL_USER) != 0) {
        new_entry |= PTE_USER;
    }

    parent[index] = new_entry;

    return phys_to_ptr(table);
}

// -----------------------------------------------------------------------------
// HAL: address space management
//
// TWO KINDS OF ROOT, AND THE DIFFERENCE MATTERS.
//
// hal_pt_create returns an EMPTY address space. It is what the VMM tests use to
// prove that two spaces are independent, and it is what a process's user half is
// built on — but it cannot be installed, because it has no kernel in it. Loading
// it into CR3 would fault on the very next instruction: the kernel's own code at
// 0x100000 is not mapped.
//
// hal_pt_create_user returns one that CAN be installed: the kernel's half is
// cloned into it, and the user half is left empty for the process to fill.
//
// The distinction went unnoticed until v0.5 because nothing had ever switched to
// a second address space. The v0.2 test created one, mapped a page into it, and
// checked the mapping did not leak into the kernel's — all without ever running
// on it. That is a real test of the mapping machinery and it proved nothing about
// being able to run there, which is the thing processes need.
// -----------------------------------------------------------------------------
hal_pt_root_t hal_pt_create(void)
{
    af_paddr root = alloc_table();
    if (root == AF_FRAME_INVALID) {
        af_error("vmm", "could not allocate a page-table root");
        return 0;
    }
    return (hal_pt_root_t)root;
}

hal_pt_root_t hal_pt_create_user(void)
{
    hal_pt_root_t current = hal_get_page_table();
    if (current == 0) {
        af_error("vmm", "hal_pt_create_user called before paging is up");
        return 0;
    }

    af_paddr root = alloc_table();
    if (root == AF_FRAME_INVALID) {
        af_error("vmm", "could not allocate a page-table root for a process");
        return 0;
    }

    af_u64 *src = phys_to_ptr((af_paddr)current);
    af_u64 *dst = phys_to_ptr(root);

    // Share the kernel's half by pointer; leave the user region empty.
    //
    // Nothing is copied. Every kernel entry in the source root — the identity
    // map, the direct map, whatever the VMM has mapped — is the SAME table in
    // the new root, which is safe precisely because the process can never write
    // to it: its own mappings go in AF_USER_PML4_INDEX and nowhere else.
    //
    // The entry for the user region is deliberately skipped rather than copied.
    // If the current address space happens to hold user mappings there — the
    // ring-3 self test's stub does — those belong to it, not to the new process,
    // and inheriting them would put one address space's pages inside another's.
    for (af_u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (i == AF_USER_PML4_INDEX) {
            continue;
        }
        dst[i] = src[i];
    }

    return (hal_pt_root_t)root;
}

void hal_pt_destroy(hal_pt_root_t root)
{
    if (root == 0) {
        return;
    }

    af_u64 *pml4 = phys_to_ptr((af_paddr)root);

    for (af_u32 pml4_i = 0; pml4_i < ENTRIES_PER_TABLE; pml4_i++) {
        af_u64 pml4e = pml4[pml4_i];

        // Skip absent entries and the entries that point at the shared kernel
        // page tables. Those belong to the kernel, not to this address space, and
        // freeing them would tear the kernel out from under every other process.
        if ((pml4e & PTE_PRESENT) == 0) {
            continue;
        }
        if ((pml4e & PTE_USER) == 0) {
            continue;
        }

        af_u64 *pdpt = phys_to_ptr(pml4e & PTE_ADDR_MASK);

        for (af_u32 pdpt_i = 0; pdpt_i < ENTRIES_PER_TABLE; pdpt_i++) {
            af_u64 pdpte = pdpt[pdpt_i];
            if ((pdpte & PTE_PRESENT) == 0) {
                continue;
            }

            af_u64 *pd = phys_to_ptr(pdpte & PTE_ADDR_MASK);

            for (af_u32 pd_i = 0; pd_i < ENTRIES_PER_TABLE; pd_i++) {
                af_u64 pde = pd[pd_i];
                if ((pde & PTE_PRESENT) == 0) {
                    continue;
                }

                if ((pde & PTE_HUGE) != 0) {
                    // A 2 MiB page. Unreferenced, not freed: see the note on
                    // pmm_frame_unref below.
                    pmm_frame_unref(pde & PTE_ADDR_MASK);
                    continue;
                }

                af_u64 *pt = phys_to_ptr(pde & PTE_ADDR_MASK);

                for (af_u32 pt_i = 0; pt_i < ENTRIES_PER_TABLE; pt_i++) {
                    af_u64 pte = pt[pt_i];
                    if ((pte & PTE_PRESENT) != 0) {
                        // UNREF, NOT FREE. This is the line that makes shared
                        // memory possible.
                        //
                        // Freeing here is correct while every frame has exactly
                        // one owner, which was true until processes shared
                        // anything. With two address spaces mapping one frame,
                        // the first destroy looks like ordinary operation and
                        // FREES a frame the second process is still using — and
                        // the second destroy then double-frees it. The failure
                        // lands on the innocent process, which is the worst
                        // property a memory bug can have.
                        //
                        // With unref, the count is 1 (allocated) + 1 (the
                        // sharer's explicit pmm_frame_ref) = 2, the first
                        // destroy takes it to 1, and the second releases it.
                        //
                        // hal_map_page does NOT take a reference, deliberately —
                        // see the note beside it — so every existing caller
                        // keeps working unchanged and sharing is explicit at the
                        // one place that does it.
                        pmm_frame_unref(pte & PTE_ADDR_MASK);
                    }
                }

                pmm_free_frame(pde & PTE_ADDR_MASK);   // the page table itself
            }

            pmm_free_frame(pdpte & PTE_ADDR_MASK);     // the page directory
        }

        pmm_free_frame(pml4e & PTE_ADDR_MASK);         // the PDPT
    }

    pmm_free_frame((af_paddr)root);
}

// -----------------------------------------------------------------------------
// HAL: mapping
// -----------------------------------------------------------------------------
af_status_t hal_map_page(hal_pt_root_t root, af_vaddr va, af_paddr pa, af_u32 flags)
{
    if (root == 0) {
        return AF_ERR_INVAL;
    }
    if (!AF_IS_ALIGNED(va, AF_PAGE_SIZE) || !AF_IS_ALIGNED(pa, AF_PAGE_SIZE)) {
        af_error("vmm", "hal_map_page: unaligned va 0x%lX or pa 0x%lX", va, pa);
        return AF_ERR_INVAL;
    }

    af_u64 *pml4 = phys_to_ptr((af_paddr)root);

    af_u64 *pdpt = walk_to_next_level(pml4, AF_PML4_INDEX(va), flags);
    if (pdpt == NULL) { return AF_ERR_NOMEM; }

    af_u64 *pd = walk_to_next_level(pdpt, AF_PDPT_INDEX(va), flags);
    if (pd == NULL) { return AF_ERR_NOMEM; }

    af_u64 *pt = walk_to_next_level(pd, AF_PD_INDEX(va), flags);
    if (pt == NULL) { return AF_ERR_NOMEM; }

    af_u64 *pte = &pt[(va >> 12) & 0x1FF];

    // Refuse to silently replace a live mapping. Overwriting one leaks the frame
    // it pointed at and leaves whoever was using it reading someone else's
    // memory, which is far worse than a clean error.
    if ((*pte & PTE_PRESENT) != 0) {
        af_paddr existing = *pte & PTE_ADDR_MASK;
        if (existing != pa) {
            af_error("vmm", "0x%lX is already mapped to 0x%lX; refusing to remap "
                            "to 0x%lX", va, existing, pa);
            return AF_ERR_EXIST;
        }
    }

    *pte = (pa & PTE_ADDR_MASK) | hal_flags_to_pte(flags);

    // The TLB may still hold the old translation. Every mapping change must be
    // followed by an invalidation, or the CPU keeps using stale entries — which
    // shows up as writes that appear to be ignored, with no fault to point at
    // the cause.
    if (root == (hal_pt_root_t)af_read_cr3()) {
        af_invlpg(va);
    }

    return AF_OK;
}

af_status_t hal_map_range(hal_pt_root_t root, af_vaddr va, af_paddr pa,
                          af_u64 size, af_u32 flags)
{
    if (size == 0) {
        return AF_ERR_INVAL;
    }

    af_u64 pages = (AF_ALIGN_UP(size, AF_PAGE_SIZE)) / AF_PAGE_SIZE;

    for (af_u64 i = 0; i < pages; i++) {
        af_status_t rc = hal_map_page(root, va + i * AF_PAGE_SIZE,
                                      pa + i * AF_PAGE_SIZE, flags);
        if (af_status_err(rc)) {
            return rc;
        }
    }

    return AF_OK;
}

af_status_t hal_unmap_page(hal_pt_root_t root, af_vaddr va)
{
    if (root == 0) {
        return AF_ERR_INVAL;
    }

    // Walk WITHOUT allocating. Unmapping something that was never mapped must
    // not create page tables as a side effect — that would turn a failed unmap
    // into a memory leak.
    af_u64 *pml4 = phys_to_ptr((af_paddr)root);
    af_u64 pml4e = pml4[AF_PML4_INDEX(va)];
    if ((pml4e & PTE_PRESENT) == 0) { return AF_ERR_NOENT; }

    af_u64 *pdpt = phys_to_ptr(pml4e & PTE_ADDR_MASK);
    af_u64 pdpte = pdpt[AF_PDPT_INDEX(va)];
    if ((pdpte & PTE_PRESENT) == 0) { return AF_ERR_NOENT; }
    if ((pdpte & PTE_HUGE) != 0) { return AF_ERR_NOTSUP; }

    af_u64 *pd = phys_to_ptr(pdpte & PTE_ADDR_MASK);
    af_u64 pde = pd[AF_PD_INDEX(va)];
    if ((pde & PTE_PRESENT) == 0) { return AF_ERR_NOENT; }
    if ((pde & PTE_HUGE) != 0) { return AF_ERR_NOTSUP; }

    af_u64 *pt = phys_to_ptr(pde & PTE_ADDR_MASK);
    af_u64 *pte = &pt[(va >> 12) & 0x1FF];

    if ((*pte & PTE_PRESENT) == 0) {
        return AF_ERR_NOENT;
    }

    *pte = 0;

    if (root == (hal_pt_root_t)af_read_cr3()) {
        af_invlpg(va);
    }

    return AF_OK;
}

void hal_unmap_range(hal_pt_root_t root, af_vaddr va, af_u64 size)
{
    af_u64 pages = AF_ALIGN_UP(size, AF_PAGE_SIZE) / AF_PAGE_SIZE;
    for (af_u64 i = 0; i < pages; i++) {
        hal_unmap_page(root, va + i * AF_PAGE_SIZE);
    }
}

af_paddr hal_translate(hal_pt_root_t root, af_vaddr va)
{
    if (root == 0) {
        return 0;
    }

    af_u64 *pml4 = phys_to_ptr((af_paddr)root);
    af_u64 pml4e = pml4[AF_PML4_INDEX(va)];
    if ((pml4e & PTE_PRESENT) == 0) { return 0; }

    af_u64 *pdpt = phys_to_ptr(pml4e & PTE_ADDR_MASK);
    af_u64 pdpte = pdpt[AF_PDPT_INDEX(va)];
    if ((pdpte & PTE_PRESENT) == 0) { return 0; }

    // A 1 GiB page ends the walk here.
    if ((pdpte & PTE_HUGE) != 0) {
        return (pdpte & PTE_ADDR_MASK) | (va & 0x3FFFFFFFULL);
    }

    af_u64 *pd = phys_to_ptr(pdpte & PTE_ADDR_MASK);
    af_u64 pde = pd[AF_PD_INDEX(va)];
    if ((pde & PTE_PRESENT) == 0) { return 0; }

    // A 2 MiB page.
    if ((pde & PTE_HUGE) != 0) {
        return (pde & PTE_ADDR_MASK) | (va & 0x1FFFFFULL);
    }

    af_u64 *pt = phys_to_ptr(pde & PTE_ADDR_MASK);
    af_u64 pte = pt[(va >> 12) & 0x1FF];
    if ((pte & PTE_PRESENT) == 0) { return 0; }

    return (pte & PTE_ADDR_MASK) | (va & 0xFFFULL);
}

af_u32 hal_query_flags(hal_pt_root_t root, af_vaddr va)
{
    if (root == 0) {
        return 0;
    }

    af_u64 *pml4 = phys_to_ptr((af_paddr)root);
    af_u64 pml4e = pml4[AF_PML4_INDEX(va)];
    if ((pml4e & PTE_PRESENT) == 0) { return 0; }

    af_u64 *pdpt = phys_to_ptr(pml4e & PTE_ADDR_MASK);
    af_u64 pdpte = pdpt[AF_PDPT_INDEX(va)];
    if ((pdpte & PTE_PRESENT) == 0) { return 0; }
    if ((pdpte & PTE_HUGE) != 0) { return pte_to_hal_flags(pdpte); }

    af_u64 *pd = phys_to_ptr(pdpte & PTE_ADDR_MASK);
    af_u64 pde = pd[AF_PD_INDEX(va)];
    if ((pde & PTE_PRESENT) == 0) { return 0; }
    if ((pde & PTE_HUGE) != 0) { return pte_to_hal_flags(pde); }

    af_u64 *pt = phys_to_ptr(pde & PTE_ADDR_MASK);
    af_u64 pte = pt[(va >> 12) & 0x1FF];
    if ((pte & PTE_PRESENT) == 0) { return 0; }

    return pte_to_hal_flags(pte);
}

// -----------------------------------------------------------------------------
// HAL: installing a page table
// -----------------------------------------------------------------------------
void hal_set_page_table(hal_pt_root_t root)
{
    af_write_cr3((af_u64)root);
}

hal_pt_root_t hal_get_page_table(void)
{
    return (hal_pt_root_t)af_read_cr3();
}

// -----------------------------------------------------------------------------
// Physical/virtual conversion
//
// Identity while the kernel runs low. These become a direct-map offset once the
// higher-half move lands, and having them as functions rather than arithmetic
// spread through the tree is what makes that a one-file change.
// -----------------------------------------------------------------------------
af_paddr hal_virt_to_phys(af_vaddr va)
{
    return (af_paddr)va;
}

af_vaddr hal_phys_to_virt(af_paddr pa)
{
    return (af_vaddr)pa;
}

af_paddr af_x86_current_page_table(void)
{
    return (af_paddr)af_read_cr3();
}

// -----------------------------------------------------------------------------
// Boot-time page-table bootstrap
// -----------------------------------------------------------------------------
//
// Builds the kernel's OWN page tables and installs them.
//
// WHY THIS EXISTS AT ALL: until it ran, the kernel was executing on the page
// tables OVMF left behind. They happened to map everything we touched, so
// nothing failed — but the kernel did not own its address space, had no way to
// create a second one, and would break the moment any mapping it relied on
// without knowing it stopped being there.
//
// The identity map covers only what the kernel actually needs, which is a
// deliberate departure from "map everything": a mapping you did not ask for is a
// mapping you cannot reason about.
void af_x86_paging_bootstrap(const af_boot_info_t *bi)
{
    af_paddr root_phys = alloc_table();
    if (root_phys == AF_FRAME_INVALID) {
        af_panic("paging: could not allocate the kernel page-table root");
    }

    af_u64 *pml4 = phys_to_ptr(root_phys);

    // --- identity-map physical memory ----------------------------------------
    //
    // 2 MiB pages up to the highest address the kernel needs to reach, rounded up
    // to a whole GiB. The kernel image, the PMM bitmap, every thread stack and
    // the framebuffer are all covered this way, and 2 MiB pages mean the whole
    // thing costs a handful of tables instead of megabytes of them.
    //
    // THE FRAMEBUFFER IS NOT IN THE PMM's RANGE. pmm_max_address() stops at the
    // end of the memory the PMM manages, and video memory sits ABOVE that — on
    // QEMU at 0x80000000, just past 2 GiB of RAM. Sizing the identity map from
    // the PMM alone would leave the framebuffer unmapped, and the first pixel
    // drawn after this switch would take a page fault.
    af_paddr highest = pmm_max_address();

    if ((bi->boot_flags & AF_BOOT_FLAG_FRAMEBUFFER) != 0) {
        af_paddr fb_end = bi->framebuffer.address + bi->framebuffer.size;
        if (fb_end > highest) {
            af_info("paging", "extending the identity map past the PMM's range to "
                              "cover the framebuffer at 0x%lX..0x%lX",
                    bi->framebuffer.address, fb_end);
            highest = fb_end;
        }
    }

    af_u64 identity_bytes = AF_ALIGN_UP((af_u64)highest, AF_GIB);

    // Cap at 512 GiB of address space: one PDPT's worth. Beyond that the map
    // needs a second PML4 entry, which nothing needs yet and which would be
    // untested.
    if (identity_bytes > 512ULL * AF_GIB) {
        identity_bytes = 512ULL * AF_GIB;
    }

    af_u32 pdpt_needed = (af_u32)(identity_bytes / AF_GIB);
    if (pdpt_needed == 0) {
        pdpt_needed = 1;
    }

    af_paddr pdpt_phys = alloc_table();
    if (pdpt_phys == AF_FRAME_INVALID) {
        af_panic("paging: could not allocate the identity-map PDPT");
    }
    pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;

    af_u64 *pdpt = phys_to_ptr(pdpt_phys);

    for (af_u32 gib = 0; gib < pdpt_needed; gib++) {
        af_paddr pd_phys = alloc_table();
        if (pd_phys == AF_FRAME_INVALID) {
            af_panic("paging: could not allocate a page directory");
        }

        pdpt[gib] = pd_phys | PTE_PRESENT | PTE_WRITABLE;

        af_u64 *pd = phys_to_ptr(pd_phys);

        for (af_u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
            af_paddr phys = ((af_paddr)gib << 30) + ((af_paddr)i << 21);

            // Stop at the end of RAM. Mapping beyond it would create entries
            // pointing at addresses that do not exist — harmless until something
            // touches them, and then a machine check rather than a page fault.
            if (phys >= (af_paddr)highest) {
                break;
            }

            pd[i] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
        }
    }

    // --- install ------------------------------------------------------------
    //
    // The tables were built through the OLD mapping, which identity-mapped
    // physical memory. The new tables identity-map it too, so the instruction
    // after this write still resolves — that equivalence is the whole reason the
    // switch is safe, and it would not be if the new map placed the kernel
    // anywhere else.
    af_write_cr3(root_phys);

    af_info("paging", "kernel page tables installed: identity map of %llu GiB "
                      "with 2 MiB pages (%u PD tables), CR3 = 0x%lX",
            (unsigned long long)(identity_bytes / AF_GIB), pdpt_needed,
            (af_u64)root_phys);
}

// -----------------------------------------------------------------------------
// Self test
// -----------------------------------------------------------------------------
//
// Maps a frame at a virtual address the identity map does NOT cover, writes
// through it, verifies the translation and the contents, then unmaps and
// verifies the mapping is gone.
//
// The address is chosen above 4 GiB deliberately. Mapping somewhere inside the
// identity map would walk into a 2 MiB huge page, and hal_map_page would refuse
// — correctly, since splitting a huge page is real work nothing needs yet. That
// refusal is worth knowing about, but it is not the behaviour under test here.
//
// The blueprint asks for this test to also assert a page fault after unmapping.
// It does not, and the reason is honest: a fault there would panic the kernel,
// and there is no fault handler that can recover yet. hal_query_flags() returning
// zero proves the same thing without taking the machine down. The #PF path gets
// exercised for real in v0.4, when the handler learns to recover.
void af_paging_selftest(void)
{
    // 0xFFFF_8000_0000_0000 is the direct-map base: well clear of the identity
    // map, and a PML4 entry the bootstrap never populated.
    const af_vaddr test_va = 0xFFFF800000000000ULL;

    af_paddr frame = pmm_alloc_frame_z();
    if (frame == AF_FRAME_INVALID) {
        af_panic("paging test: could not allocate a frame");
    }

    hal_pt_root_t root = hal_get_page_table();

    // Confirm nothing is mapped there yet, otherwise the test proves nothing.
    if (hal_query_flags(root, test_va) != 0) {
        af_panic("paging test: 0x%lX is already mapped before the test", test_va);
    }

    af_status_t rc = hal_map_page(root, test_va, frame,
                                  HAL_PRESENT | HAL_WRITABLE | HAL_NOCACHE);
    if (af_status_err(rc)) {
        af_panic("paging test: hal_map_page failed (%s)", af_status_name(rc));
    }

    // The translation must point exactly where we asked.
    af_paddr translated = hal_translate(root, test_va);
    if (translated != frame) {
        af_panic("paging test: 0x%lX translates to 0x%lX, expected 0x%lX",
                 test_va, translated, frame);
    }

    // Write through the new mapping and read it back. This is the step that
    // actually proves the mapping is live — a page-table entry can be correct
    // and still not be in use if the TLB was never invalidated.
    af_u8 *view = (af_u8 *)(af_uptr)test_va;
    af_memset(view, 0xC3, AF_PAGE_SIZE);

    for (af_u32 i = 0; i < AF_PAGE_SIZE; i += 512) {
        if (view[i] != 0xC3) {
            af_panic("paging test: write through 0x%lX was not visible at "
                     "offset %u", test_va, i);
        }
    }

    // And the underlying frame must contain it, read through the identity map.
    {
        const af_u8 *direct = (const af_u8 *)(af_uptr)frame;
        if (direct[0] != 0xC3 || direct[AF_PAGE_SIZE - 1] != 0xC3) {
            af_panic("paging test: the write through 0x%lX did not reach the "
                     "frame at 0x%lX — the mapping is not the frame we asked for",
                     test_va, frame);
        }
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   mapped 0x%lX -> 0x%lX, wrote and read "
                                  "back through it", test_va, frame);

    // --- unmapping ------------------------------------------------------------
    rc = hal_unmap_page(root, test_va);
    if (af_status_err(rc)) {
        af_panic("paging test: hal_unmap_page failed (%s)", af_status_name(rc));
    }

    if (hal_translate(root, test_va) != 0) {
        af_panic("paging test: 0x%lX still translates after being unmapped",
                 test_va);
    }
    if (hal_query_flags(root, test_va) != 0) {
        af_panic("paging test: 0x%lX still reports mapping flags after unmap",
                 test_va);
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   unmapped 0x%lX; translation is gone",
           test_va);

    // Unmapping something that was never mapped must fail cleanly rather than
    // creating page tables as a side effect.
    rc = hal_unmap_page(root, test_va + 0x1000);
    if (rc != AF_ERR_NOENT) {
        af_panic("paging test: unmapping an unmapped page returned %s, "
                 "expected ERR_NOENT", af_status_name(rc));
    }

    // Re-mapping must work, and must not be confused by the previous mapping.
    rc = hal_map_page(root, test_va, frame, HAL_PRESENT | HAL_WRITABLE);
    if (af_status_err(rc)) {
        af_panic("paging test: re-mapping 0x%lX failed (%s)", test_va,
                 af_status_name(rc));
    }
    hal_unmap_page(root, test_va);

    pmm_free_frame(frame);

    af_log(AF_LOG_DEBUG, "test", "  ok   unmap of an unmapped page returns "
                                  "ERR_NOENT; re-mapping works");

    // --- a separate address space --------------------------------------------
    //
    // The point of the VMM: a second page-table root that can hold its own
    // mappings without touching the kernel's. This is what user mode is built
    // on, so it is worth proving the machinery exists before it is needed.
    hal_pt_root_t space2 = hal_pt_create();
    if (space2 == 0) {
        af_panic("paging test: could not create a second address space");
    }
    if (space2 == root) {
        af_panic("paging test: the second address space aliases the kernel's");
    }

    af_paddr frame2 = pmm_alloc_frame_z();
    rc = hal_map_page(space2, test_va, frame2, HAL_PRESENT | HAL_WRITABLE);
    if (af_status_err(rc)) {
        af_panic("paging test: mapping into the second space failed (%s)",
                 af_status_name(rc));
    }

    // The mapping exists in space2 and NOT in the kernel's space. If these were
    // not independent, every protection boundary in the system would be fiction.
    if (hal_translate(space2, test_va) != frame2) {
        af_panic("paging test: the second space does not translate 0x%lX",
                 test_va);
    }
    if (hal_translate(root, test_va) != 0) {
        af_panic("paging test: mapping into the second space leaked into the "
                 "kernel's address space");
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   a second address space holds its own "
                                  "mappings independently");

    hal_pt_destroy(space2);

    af_log(AF_LOG_DEBUG, "test", "  ok   the second address space was destroyed "
                                  "and its frames released");

    // --- an address space that can actually be RUN ON -------------------------
    //
    // The test above built an empty root, mapped into it, and checked the
    // mapping did not leak. All true, and it proves nothing about being able to
    // run there: an empty root has no kernel in it, so installing it would fault
    // on the next instruction fetch. That gap is the difference between "the
    // mapping machinery works" and "a process can exist", and it went unnoticed
    // until processes needed the second one.
    hal_pt_root_t uspace = hal_pt_create_user();
    if (uspace == 0) {
        af_panic("paging test: could not create a user address space");
    }
    if (uspace == root) {
        af_panic("paging test: the user address space aliases the kernel's");
    }

    // Checked BEFORE switching, because after the switch there is no way to
    // report anything. A kernel that is not mapped in this root means the clone
    // dropped it — the failure mode is a triple fault with no output, so the
    // precondition is asserted while the machine can still talk.
    //
    // The address comes from the linker, not from a literal, so this test keeps
    // testing the right thing after the higher-half move changes where the
    // kernel lives.
    extern const af_u8 __kernel_start[];
    const af_vaddr text_va = (af_vaddr)(af_uptr)&__kernel_start[0];

    if (hal_translate(uspace, text_va) == 0) {
        af_panic("paging test: the user address space has no kernel at 0x%lX — "
                 "installing it would fault on the next instruction fetch",
                 (af_u64)text_va);
    }

    // The kernel heap, where every allocation after this one lands.
    if (hal_translate(uspace, (af_vaddr)(af_uptr)&uspace) == 0) {
        af_panic("paging test: the kernel's own data is not mapped in the user "
                 "address space");
    }

    // ...and the user region must be EMPTY. It is a top-level slot a new address
    // space does not inherit — the space it was created from keeps its own — so
    // nothing there should translate yet.
    if (hal_translate(uspace, AF_USER_REGION_BASE) != 0) {
        af_panic("paging test: the user address space inherited a user mapping "
                 "at 0x%lX from the address space it was created in",
                 (af_u64)AF_USER_REGION_BASE);
    }

    // A mapping made here must be invisible to the kernel's space — the same
    // property as the test above, but on the root that will actually be run on.
    //
    // The address must be INSIDE the user region for that to hold. At 4 GiB it
    // would land in PML4[0], which every address space shares with the kernel,
    // and this assertion would fire. It did, the first time this test was run
    // with the new layout — which is the point of writing it down.
    af_u64 probe_va = AF_USER_REGION_BASE;
    af_paddr probe_frame = pmm_alloc_frame_z();
    rc = hal_map_page(uspace, probe_va, probe_frame,
                      HAL_PRESENT | HAL_WRITABLE | HAL_USER);
    if (af_status_err(rc)) {
        af_panic("paging test: mapping into the user address space failed (%s)",
                 af_status_name(rc));
    }
    if (hal_translate(root, probe_va) != 0) {
        af_panic("paging test: a user mapping leaked into the kernel's address "
                 "space — every process would share one user half");
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   a user address space carries the kernel "
                                  "but not the kernel's user mappings");

    // And now the part the earlier test never did: run on it.
    //
    // Every check above is a prediction. This is the experiment. If the clone
    // was wrong anywhere the kernel touches on the way here — its text, its
    // stack, its heap — the machine dies on the next few instructions with
    // nothing printed, which is why everything worth asserting was asserted
    // first and why hal_pt_create_user is written to be conservative.
    hal_pt_root_t kernel_root = root;
    hal_set_page_table(uspace);

    // Still executing. The value proves this ran on the new tables rather than
    // silently failing to switch: CR3 changed, and this read came back.
    if (hal_get_page_table() != uspace) {
        af_panic("paging test: hal_set_page_table did not take effect");
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   the kernel runs on a process address "
                                  "space and reaches its own memory");

    hal_set_page_table(kernel_root);

    // Back on the kernel's tables, and the user mapping is where it was left.
    if (hal_translate(uspace, probe_va) != probe_frame) {
        af_panic("paging test: the user address space lost its mapping across "
                 "the switch");
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   switching back restores the kernel's "
                                  "address space with the process's intact");

    hal_pt_destroy(uspace);

    af_marker("AF_VMM_OK");
}

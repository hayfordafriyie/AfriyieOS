// SPDX-License-Identifier: MIT
// AfriyieOS — Physical Memory Manager
//
// Tracks every 4 KiB frame in the machine with a bitmap, and reference counts
// each frame so that copy-on-write fork (v0.4) and shared memory grants (v0.7)
// can both work without retrofitting.
//
// The PMM's entire view of the machine is the normalised memory map in
// af_boot_info. It knows nothing about UEFI or Device Trees — that is the boot
// bridge's job, and keeping it that way is what lets the same code run on a
// phone.

#ifndef AFRIYIE_PMM_H
#define AFRIYIE_PMM_H

#include "types.h"
#include "status.h"
#include "boot_info.h"

// One frame is one page. 4 KiB on every platform AfriyieOS targets.
#define AF_FRAME_SIZE   AF_PAGE_SIZE
#define AF_FRAME_SHIFT  AF_PAGE_SHIFT

// Physical address 0 is never a valid frame (the boot map reserves it), so it is
// a safe "allocation failed" sentinel.
#define AF_FRAME_INVALID ((af_paddr)0)

typedef struct {
    af_u64 total_bytes;       // every frame the map describes
    af_u64 usable_bytes;      // frames the PMM may hand out
    af_u64 free_bytes;        // frames currently free
    af_u64 used_bytes;        // usable - free
    af_u64 reserved_bytes;    // frames deliberately held back
    af_u32 total_frames;
    af_u32 free_frames;
    af_u32 used_frames;
    af_u32 largest_free_run;  // biggest contiguous free span, in frames
    af_u32 bitmap_bytes;
    af_u32 refcount_bytes;
} af_pmm_stats_t;

// Builds the bitmap from the boot handoff. Everything starts marked USED — the
// safe default, so a bug in the region loop leaks memory rather than handing out
// firmware or video memory — and then usable regions are freed and the kernel's
// own occupancy is re-reserved.
af_status_t pmm_init(const af_boot_info_t *bi);

// Allocate a single frame. Returns AF_FRAME_INVALID on failure.
// The frame is reference-counted at 1 and is NOT zeroed (see pmm_alloc_frame_z).
af_paddr pmm_alloc_frame(void);

// Allocate one frame, zeroed. The zeroing is unconditional, not debug-only:
// handing a caller a frame containing another process's data is a security bug,
// not a debugging inconvenience.
af_paddr pmm_alloc_frame_z(void);

// Allocate `count` physically contiguous frames. Returns AF_FRAME_INVALID on
// failure. Used for DMA buffers and for large kernel heap allocations.
af_paddr pmm_alloc_frames(af_u32 count);

void pmm_free_frame(af_paddr frame);
void pmm_free_frames(af_paddr base, af_u32 count);

// --- reference counting ------------------------------------------------------
void    pmm_frame_ref(af_paddr frame);
void    pmm_frame_unref(af_paddr frame);   // frees the frame at zero
af_u32  pmm_frame_refcount(af_paddr frame);

// --- reservation -------------------------------------------------------------
// Marks a physical range used, permanently. Used during init for the kernel
// image, the bitmap itself and the framebuffer, and later for MMIO windows.
void pmm_reserve_range(af_paddr base, af_u64 size, const char *why);
void pmm_release_range(af_paddr base, af_u64 size);

// --- queries -----------------------------------------------------------------
af_paddr pmm_max_address(void);
void     pmm_stats(af_pmm_stats_t *out);
void     pmm_dump_stats(void);

// In-kernel self test, run from af_selftest_run_all(). Prints AF_TEST_FAIL:<name>
// and panics on any failure.
void pmm_selftest(void);

#endif // AFRIYIE_PMM_H

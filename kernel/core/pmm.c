// SPDX-License-Identifier: MIT
// AfriyieOS — Physical Memory Manager
//
// =============================================================================
// DESIGN
// =============================================================================
// One bit per 4 KiB frame: 1 means allocated, 0 means free. Plus one byte per
// frame for the reference count.
//
//   4 GiB RAM  -> 1 048 576 frames -> 128 KiB of bitmap + 1 MiB of refcounts
//   64 GiB RAM -> 16 777 216 frames -> 2 MiB of bitmap + 16 MiB of refcounts
//
// That is an acceptable price for O(1) allocation, metadata that cannot
// fragment, and a debug story that reduces to a popcount. A free-list allocator
// would use less metadata and be much harder to prove correct.
//
// The metadata lives in RAM the map says is usable, placed by pmm_init() and
// then reserved in the bitmap itself. There is no fixed-size static array, so
// the design does not quietly cap how much RAM a machine may have.
// =============================================================================

#include "afriyie/pmm.h"
#include "afriyie/config.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/hal.h"

// Linker-provided image bounds. Used to work out how much memory the kernel
// actually occupies — see the reservation of the kernel image below, which is
// where this matters enormously.
extern const af_u8 __kernel_start[];
extern const af_u8 __kernel_end[];

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static af_u8  *s_bitmap   = NULL;   // 1 bit per frame, 1 = used
static af_u8  *s_refcount = NULL;   // 1 byte per frame
static af_u32  s_frame_count = 0;   // frames the map describes
static af_u32  s_usable_frames = 0;
static af_u32  s_free_frames = 0;
static af_u32  s_search_hint = 0;   // where the last successful scan ended

// A frame refcount of 0 means free; 1 means one owner; higher means shared.
// The count saturates rather than wrapping: a wrapped refcount frees a frame
// that is still in use, which corrupts memory far from the cause.
#define AF_REFCOUNT_MAX 0xFFu

// -----------------------------------------------------------------------------
// Bitmap primitives
// -----------------------------------------------------------------------------
AF_INLINE bool frame_is_used(af_u32 frame)
{
    return (s_bitmap[frame >> 3] & (af_u8)(1u << (frame & 7u))) != 0;
}

AF_INLINE void frame_mark_used(af_u32 frame)
{
    s_bitmap[frame >> 3] |= (af_u8)(1u << (frame & 7u));
}

AF_INLINE void frame_mark_free(af_u32 frame)
{
    s_bitmap[frame >> 3] &= (af_u8)~(1u << (frame & 7u));
}

AF_INLINE af_u32 addr_to_frame(af_paddr pa)
{
    return (af_u32)(pa >> AF_FRAME_SHIFT);
}

AF_INLINE af_paddr frame_to_addr(af_u32 frame)
{
    return (af_paddr)frame << AF_FRAME_SHIFT;
}

// -----------------------------------------------------------------------------
// Range marking
//
// Both helpers clamp to the frames the map actually describes, so a caller can
// pass a region that runs off the end of RAM without corrupting the bitmap.
// -----------------------------------------------------------------------------
static void mark_range_free(af_paddr base, af_u64 size)
{
    if (size == 0) {
        return;
    }

    af_u32 first = addr_to_frame(AF_ALIGN_UP(base, AF_FRAME_SIZE));
    af_u32 last  = addr_to_frame(AF_ALIGN_DOWN(base + size, AF_FRAME_SIZE));

    if (last > s_frame_count) {
        last = s_frame_count;
    }

    for (af_u32 f = first; f < last; f++) {
        if (frame_is_used(f)) {
            frame_mark_free(f);
        }
    }
}

void pmm_reserve_range(af_paddr base, af_u64 size, const char *why)
{
    if (s_bitmap == NULL || size == 0) {
        return;
    }

    af_u32 before = 0;
    af_u32 first = addr_to_frame(AF_ALIGN_DOWN(base, AF_FRAME_SIZE));
    af_u32 last  = addr_to_frame(AF_ALIGN_UP(base + size, AF_FRAME_SIZE));
    if (last > s_frame_count) {
        last = s_frame_count;
    }

    for (af_u32 f = first; f < last; f++) {
        if (!frame_is_used(f)) {
            before++;
            frame_mark_used(f);
            s_refcount[f] = 0;
        }
    }

    if (before > 0) {
        s_free_frames -= before;
        af_info("pmm", "reserved %u frame(s) at 0x%lX for %s",
                before, base, (why != NULL) ? why : "unspecified");
    }
}

void pmm_release_range(af_paddr base, af_u64 size)
{
    if (s_bitmap == NULL || size == 0) {
        return;
    }

    af_u32 first = addr_to_frame(AF_ALIGN_UP(base, AF_FRAME_SIZE));
    af_u32 last  = addr_to_frame(AF_ALIGN_DOWN(base + size, AF_FRAME_SIZE));
    if (last > s_frame_count) {
        last = s_frame_count;
    }

    for (af_u32 f = first; f < last; f++) {
        if (frame_is_used(f) && s_refcount[f] == 0) {
            frame_mark_free(f);
            s_free_frames++;
        }
    }
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
af_status_t pmm_init(const af_boot_info_t *bi)
{
    if (bi == NULL) {
        return AF_ERR_INVAL;
    }

    // Size the bitmap from the highest USABLE address, not the highest address
    // in the whole map. Firmware describes device and reserved windows far above
    // RAM — QEMU's map reaches 1 TiB on a 2 GiB machine — and sizing from that
    // produced 288 MiB of metadata and initialisation loops that walked 268
    // million frames which can never be allocated.
    af_paddr highest = af_boot_info_max_usable_address(bi);
    if (highest == 0) {
        return AF_ERR_BOOT_MEMMAP;
    }

    s_frame_count = addr_to_frame(AF_ALIGN_DOWN(highest, AF_FRAME_SIZE));
    if (s_frame_count < 1024) {
        af_error("pmm", "only %u frames described — refusing to manage this map",
                 s_frame_count);
        return AF_ERR_BOOT_MEMMAP;
    }

    af_u32 bitmap_bytes   = (s_frame_count + 7) / 8;
    af_u32 refcount_bytes = s_frame_count;

    // Round the bitmap up to a frame boundary so it can be reserved as whole
    // frames. The refcount array is placed immediately after it.
    af_u32 bitmap_frames = (bitmap_bytes + AF_FRAME_SIZE - 1) / AF_FRAME_SIZE;
    bitmap_bytes = bitmap_frames * AF_FRAME_SIZE;

    af_u64 metadata_bytes = (af_u64)bitmap_bytes + refcount_bytes;
    af_u32 metadata_frames = (af_u32)((metadata_bytes + AF_FRAME_SIZE - 1) / AF_FRAME_SIZE);

    af_info("pmm", "managing %u frames (%llu MiB) with %u KiB of metadata",
            s_frame_count, (unsigned long long)(((af_u64)s_frame_count * AF_FRAME_SIZE) / AF_MIB),
            (af_u32)(metadata_frames * AF_FRAME_SIZE / AF_KIB));

    // --- find somewhere to put the metadata ----------------------------------
    //
    // It must be inside a single usable region: a bitmap straddling a hole in
    // the map would be partly unusable memory. Pick the largest usable region
    // and place the metadata at its start.
    af_paddr region_base = 0;
    af_u64   region_size = 0;

    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        const af_memory_region_t *r = &bi->memory_regions[i];
        if (r->type != AF_MEM_USABLE) {
            continue;
        }
        // Skip anything overlapping the kernel image, which we are executing
        // from and must not overwrite.
        if (r->base < bi->kernel_phys_base + bi->kernel_phys_size &&
            r->base + r->length > bi->kernel_phys_base) {
            continue;
        }
        if (r->length > region_size) {
            region_size = r->length;
            region_base = r->base;
        }
    }

    if (region_base == 0 || region_size < metadata_bytes + AF_MIB) {
        af_error("pmm", "no usable region large enough for %llu KiB of metadata",
                 metadata_bytes / AF_KIB);
        return AF_ERR_NOMEM;
    }

    s_bitmap   = (af_u8 *)(af_uptr)region_base;
    s_refcount = s_bitmap + bitmap_bytes;

    // --- everything starts USED ----------------------------------------------
    //
    // The default must be the safe one. If the region loop below has a bug, the
    // result is leaked memory, not a frame of firmware or video RAM handed to an
    // unsuspecting caller.
    af_memset(s_bitmap, 0xFF, bitmap_bytes);
    af_memset(s_refcount, 0, refcount_bytes);

    s_free_frames   = 0;
    s_usable_frames = 0;
    s_search_hint   = 0;

    // --- free what may be used ------------------------------------------------
    //
    // AF_MEM_BOOTLOADER is the loader's own code and data. After
    // ExitBootServices that memory is genuinely free — with one enormous
    // exception: our kernel image is reported as "bootloader" memory, and we are
    // executing from it. It is re-reserved below, along with everything else
    // that must survive.
    for (af_u32 i = 0; i < bi->memory_region_count; i++) {
        const af_memory_region_t *r = &bi->memory_regions[i];

        if (r->type != AF_MEM_USABLE && r->type != AF_MEM_BOOTLOADER) {
            continue;
        }

        mark_range_free(r->base, r->length);
    }

    // Count what we just freed, then subtract the reservations below.
    for (af_u32 f = 0; f < s_frame_count; f++) {
        if (!frame_is_used(f)) {
            s_free_frames++;
            s_usable_frames++;
        }
    }

    af_info("pmm", "usable: %u frames (%llu MiB)",
            s_usable_frames,
            (unsigned long long)(((af_u64)s_usable_frames * AF_FRAME_SIZE) / AF_MIB));

    // --- reserve what must survive -------------------------------------------
    //
    // ORDER MATTERS AND EACH ENTRY IS LOAD-BEARING. Every one of these was
    // observed in the real memory map during the first successful boot.

    // 1. The null page. Physical address 0 is used as the "no frame" sentinel,
    //    so it must never be handed out.
    pmm_reserve_range(0, AF_FRAME_SIZE, "null page");

    // 2. The boot handoff backup at 0x7000. Firmware reports the low megabyte as
    //    USABLE, so this would otherwise be handed out on the first allocation —
    //    and with the kernel's fallback path reading it, that is a live
    //    corruption bug, not a theoretical one.
    pmm_reserve_range(AF_BOOT_INFO_BACKUP_ADDR, sizeof(af_boot_info_t),
                      "boot_info backup");

    // 3. The kernel image — AND THIS IS THE ONE THAT BITES.
    //
    // The boot bridge reports the size of the FLAT BINARY it copied, and a flat
    // binary produced by `objcopy -O binary` does not contain NOBITS sections.
    // So `kernel_phys_size` covers .text, .rodata and .data — and omits .bss
    // entirely, even though .bss occupies real memory that the kernel is using.
    //
    // On the first v0.2 boot that gap was 111 KiB. The PMM freed the kernel's own
    // .bss, the heap self test allocated a block there, filled it with 0x18, and
    // overwrote s_bitmap — after which the next pmm_alloc_frames read a bitmap
    // pointer of 0x1818181818181818 and took a general protection fault. The
    // symptom was three function calls away from the cause, which is exactly how
    // allocator bugs behave.
    //
    // The kernel knows its own extent from the linker script, so the larger of
    // the two figures wins. That stays correct even once the kernel is mapped in
    // the higher half, where the reported physical size and the linker symbols
    // stop being directly comparable and this check has to change shape.
    af_paddr kernel_start = (af_paddr)(af_uptr)&__kernel_start[0];
    af_paddr kernel_end   = (af_paddr)(af_uptr)&__kernel_end[0];
    af_paddr reported_end = bi->kernel_phys_base + bi->kernel_phys_size;

    if (kernel_end < reported_end) {
        kernel_end = reported_end;
    }

    if (kernel_end > kernel_start) {
        af_u64 size = (af_u64)(kernel_end - kernel_start);
        pmm_reserve_range(kernel_start, size,
                          "kernel image (.text .rodata .data .bss)");
    }

    // 4. The PMM's own metadata.
    pmm_reserve_range(region_base, metadata_bytes, "pmm metadata");

    // 5. The framebuffer. The boot bridge already re-typed the region, but
    //    reserving explicitly means a firmware that reports video memory as
    //    conventional cannot cost us the screen.
    if ((bi->boot_flags & AF_BOOT_FLAG_FRAMEBUFFER) != 0) {
        pmm_reserve_range(bi->framebuffer.address, bi->framebuffer.size,
                          "framebuffer");
    }

    // 6. The ramdisk, when there is one.
    if ((bi->boot_flags & AF_BOOT_FLAG_RAMDISK) != 0 && bi->ramdisk_size > 0) {
        pmm_reserve_range(bi->ramdisk_phys_base, bi->ramdisk_size, "ramdisk");
    }

    s_search_hint = 0;

    af_info("pmm", "ready: %u frames free of %u usable (%llu MiB available)",
            s_free_frames, s_usable_frames,
            (unsigned long long)(((af_u64)s_free_frames * AF_FRAME_SIZE) / AF_MIB));

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Allocation
// -----------------------------------------------------------------------------
af_paddr pmm_alloc_frames(af_u32 count)
{
    if (s_bitmap == NULL || count == 0) {
        return AF_FRAME_INVALID;
    }

    if (count > s_free_frames) {
        return AF_FRAME_INVALID;
    }

    // A single frame is by far the common case and does not need a contiguous
    // run search.
    if (count == 1) {
        for (af_u32 i = 0; i < s_frame_count; i++) {
            // Start from the hint rather than from frame 0. Without it every
            // allocation re-scans the whole bitmap, which is the difference
            // between a few hundred cycles and tens of thousands.
            af_u32 f = (s_search_hint + i) % s_frame_count;
            if (!frame_is_used(f)) {
                frame_mark_used(f);
                s_refcount[f] = 1;
                s_free_frames--;
                s_search_hint = (f + 1) % s_frame_count;
                return frame_to_addr(f);
            }
        }
        return AF_FRAME_INVALID;
    }

    // Contiguous run: first fit, from the beginning. Deliberately not hinted —
    // a run search from a rotating hint can thrash across the whole bitmap.
    af_u32 run_start = 0;
    af_u32 run_length = 0;

    for (af_u32 f = 0; f < s_frame_count; f++) {
        if (frame_is_used(f)) {
            run_length = 0;
            continue;
        }
        if (run_length == 0) {
            run_start = f;
        }
        run_length++;

        if (run_length == count) {
            for (af_u32 k = run_start; k < run_start + count; k++) {
                frame_mark_used(k);
                s_refcount[k] = 1;
            }
            s_free_frames -= count;
            return frame_to_addr(run_start);
        }
    }

    return AF_FRAME_INVALID;
}

af_paddr pmm_alloc_frame(void)
{
    return pmm_alloc_frames(1);
}

af_paddr pmm_alloc_frame_z(void)
{
    af_paddr frame = pmm_alloc_frame();
    if (frame != AF_FRAME_INVALID) {
        // Identity-mapped at v0.2: the physical address is directly writable.
        // Once the VMM lands, this becomes a memset through the direct map.
        af_memset((void *)(af_uptr)frame, 0, AF_FRAME_SIZE);
    }
    return frame;
}

// -----------------------------------------------------------------------------
// Freeing
// -----------------------------------------------------------------------------
static bool frame_valid(af_paddr frame)
{
    if (s_bitmap == NULL || frame == AF_FRAME_INVALID) {
        return false;
    }
    if (!AF_IS_ALIGNED(frame, AF_FRAME_SIZE)) {
        return false;
    }
    return addr_to_frame(frame) < s_frame_count;
}

void pmm_free_frame(af_paddr frame)
{
    if (!frame_valid(frame)) {
        af_warn("pmm", "pmm_free_frame(0x%lX) ignored: not a managed frame", frame);
        return;
    }

    af_u32 f = addr_to_frame(frame);

    if (!frame_is_used(f)) {
        // A double free is a bug in the caller. Reported rather than silently
        // absorbed, because absorbing it hides the real defect and lets the
        // frame be handed to two owners later.
        af_error("pmm", "double free of frame 0x%lX", frame);
        AF_ASSERT_MSG(false, "double free of frame 0x%lX", frame);
        return;
    }

    if (s_refcount[f] > 1) {
        af_error("pmm", "free of frame 0x%lX with %u references still held",
                 frame, s_refcount[f]);
        AF_ASSERT_MSG(false, "free of a referenced frame");
        return;
    }

    frame_mark_free(f);
    s_refcount[f] = 0;
    s_free_frames++;

    if (addr_to_frame(frame) < s_search_hint) {
        s_search_hint = addr_to_frame(frame);
    }
}

void pmm_free_frames(af_paddr base, af_u32 count)
{
    for (af_u32 i = 0; i < count; i++) {
        pmm_free_frame(base + (af_paddr)i * AF_FRAME_SIZE);
    }
}

// -----------------------------------------------------------------------------
// Reference counting
// -----------------------------------------------------------------------------
void pmm_frame_ref(af_paddr frame)
{
    if (!frame_valid(frame)) {
        return;
    }

    af_u32 f = addr_to_frame(frame);

    if (s_refcount[f] == AF_REFCOUNT_MAX) {
        // Saturation rather than wrap. A wrapped count reaching zero would free
        // a frame that still has live owners.
        af_error("pmm", "refcount saturation on frame 0x%lX", frame);
        return;
    }

    s_refcount[f]++;
}

void pmm_frame_unref(af_paddr frame)
{
    if (!frame_valid(frame)) {
        return;
    }

    af_u32 f = addr_to_frame(frame);

    if (s_refcount[f] == 0) {
        af_warn("pmm", "unref of an unreferenced frame 0x%lX", frame);
        return;
    }

    s_refcount[f]--;

    if (s_refcount[f] == 0) {
        frame_mark_free(f);
        s_free_frames++;
    }
}

af_u32 pmm_frame_refcount(af_paddr frame)
{
    if (!frame_valid(frame)) {
        return 0;
    }
    return s_refcount[addr_to_frame(frame)];
}

// -----------------------------------------------------------------------------
// Queries
// -----------------------------------------------------------------------------
af_paddr pmm_max_address(void)
{
    return frame_to_addr(s_frame_count);
}

void pmm_stats(af_pmm_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    af_memset(out, 0, sizeof(*out));

    out->total_frames = s_frame_count;
    out->total_bytes  = (af_u64)s_frame_count * AF_FRAME_SIZE;
    out->free_frames  = s_free_frames;
    out->free_bytes   = (af_u64)s_free_frames * AF_FRAME_SIZE;
    out->used_frames  = s_frame_count - s_free_frames;
    out->used_bytes   = (af_u64)out->used_frames * AF_FRAME_SIZE;
    out->usable_bytes = (af_u64)s_usable_frames * AF_FRAME_SIZE;
    out->reserved_bytes = out->total_bytes - out->usable_bytes;
    out->bitmap_bytes   = (s_frame_count + 7) / 8;
    out->refcount_bytes = s_frame_count;

    // Largest contiguous free run. Useful as an early fragmentation signal: a
    // falling figure under a stable allocation count means the allocator is
    // scattering, which will eventually break a large contiguous request.
    af_u32 longest = 0;
    af_u32 current = 0;
    for (af_u32 f = 0; f < s_frame_count; f++) {
        if (frame_is_used(f)) {
            current = 0;
            continue;
        }
        current++;
        if (current > longest) {
            longest = current;
        }
    }
    out->largest_free_run = longest;
}

void pmm_dump_stats(void)
{
    af_pmm_stats_t s;
    pmm_stats(&s);

    char human[24];
    af_format_bytes_human(s.free_bytes, human, sizeof(human));

    af_info("pmm", "frames  : %u total, %u free, %u used",
            s.total_frames, s.free_frames, s.used_frames);
    af_info("pmm", "memory  : %s free of %llu MiB total",
            human, (unsigned long long)(s.total_bytes / AF_MIB));
    af_info("pmm", "largest free run: %u frames (%llu MiB)",
            s.largest_free_run,
            (unsigned long long)(((af_u64)s.largest_free_run * AF_FRAME_SIZE) / AF_MIB));
    af_info("pmm", "metadata: %u KiB bitmap + %u KiB refcounts",
            s.bitmap_bytes / 1024, s.refcount_bytes / 1024);
}

// -----------------------------------------------------------------------------
// Self test
// -----------------------------------------------------------------------------
//
// The test that matters is the last one: allocate ten thousand frames, free
// them, and assert the free count returns exactly to where it started. A bitmap
// allocator that leaks one frame per allocation passes every casual test and
// fails this one immediately.
void pmm_selftest(void)
{
    af_pmm_stats_t before;
    pmm_stats(&before);

    // --- allocate and free a single frame -----------------------------------
    af_paddr a = pmm_alloc_frame();
    if (a == AF_FRAME_INVALID) {
        af_error("test", "  FAIL pmm_alloc_frame returned no frame");
        af_log_raw(AF_BOOT_MARKER_FAIL "pmm_alloc_frame\n");
        af_panic("pmm self test: allocation failed");
    }

    af_pmm_stats_t mid;
    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames - 1) {
        af_panic("pmm self test: one allocation consumed %u frames",
                 before.free_frames - mid.free_frames);
    }

    if (AF_IS_ALIGNED(a, AF_FRAME_SIZE) == false) {
        af_panic("pmm self test: frame 0x%lX is not page aligned", a);
    }

    pmm_free_frame(a);
    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames) {
        af_panic("pmm self test: free did not restore the frame count "
                 "(%u vs %u)", mid.free_frames, before.free_frames);
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   pmm single frame alloc/free is exact");

    // --- zeroed allocation ---------------------------------------------------
    af_paddr z = pmm_alloc_frame_z();
    if (z == AF_FRAME_INVALID) {
        af_panic("pmm self test: zeroed allocation failed");
    }
    {
        const af_u8 *p = (const af_u8 *)(af_uptr)z;
        bool all_zero = true;
        for (af_u32 i = 0; i < AF_FRAME_SIZE; i++) {
            if (p[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (!all_zero) {
            af_panic("pmm self test: pmm_alloc_frame_z returned dirty memory");
        }
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   pmm_alloc_frame_z returns zeroed memory");

    // Write, free, reallocate: the frame must come back zeroed again.
    af_memset((void *)(af_uptr)z, 0xAA, AF_FRAME_SIZE);
    pmm_free_frame(z);
    af_paddr z2 = pmm_alloc_frame_z();
    {
        const af_u8 *p = (const af_u8 *)(af_uptr)z2;
        if (p[0] != 0 || p[AF_FRAME_SIZE - 1] != 0) {
            af_panic("pmm self test: a reused frame was not zeroed");
        }
    }
    pmm_free_frame(z2);
    af_log(AF_LOG_DEBUG, "test", "  ok   a recycled frame is zeroed again");

    // --- contiguous run ------------------------------------------------------
    af_paddr run = pmm_alloc_frames(16);
    if (run == AF_FRAME_INVALID) {
        af_panic("pmm self test: 16-frame contiguous allocation failed");
    }
    if (!AF_IS_ALIGNED(run, AF_FRAME_SIZE)) {
        af_panic("pmm self test: contiguous run is not page aligned");
    }
    // Every frame in the run must be usable without walking into a neighbour.
    af_memset((void *)(af_uptr)run, 0x5A, 16 * AF_FRAME_SIZE);
    pmm_free_frames(run, 16);

    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames) {
        af_panic("pmm self test: contiguous alloc/free leaked %d frames",
                 (int)before.free_frames - (int)mid.free_frames);
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   16-frame contiguous run alloc/free is exact");

    // --- reference counting --------------------------------------------------
    af_paddr r = pmm_alloc_frame();
    if (pmm_frame_refcount(r) != 1) {
        af_panic("pmm self test: a fresh frame has refcount %u, expected 1",
                 pmm_frame_refcount(r));
    }
    pmm_frame_ref(r);
    pmm_frame_ref(r);
    if (pmm_frame_refcount(r) != 3) {
        af_panic("pmm self test: refcount is %u after three refs",
                 pmm_frame_refcount(r));
    }

    pmm_frame_unref(r);
    pmm_frame_unref(r);
    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames - 1) {
        af_panic("pmm self test: the frame was freed before its last unref");
    }

    pmm_frame_unref(r);
    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames) {
        af_panic("pmm self test: unref to zero did not free the frame");
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   refcount holds a frame until the last unref");

    // --- the bulk test: 10 000 frames, allocated and returned ----------------
    //
    // This is the one that catches a leak of a frame per allocation, a
    // refcount that is not cleared, or a search hint that skips a frame.
    enum { BULK = 10000 };
    static af_paddr bulk[BULK];

    af_u32 allocated = 0;
    for (af_u32 i = 0; i < BULK; i++) {
        af_paddr f = pmm_alloc_frame();
        if (f == AF_FRAME_INVALID) {
            break;   // out of memory is not a failure; the machine is small
        }
        bulk[i] = f;
        allocated++;
    }

    if (allocated < 1000) {
        af_panic("pmm self test: only allocated %u of %u frames — the machine "
                 "reports far more usable memory than that", allocated, (af_u32)BULK);
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   allocated %u frames in bulk", allocated);

    // The count must have dropped by exactly the number allocated.
    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames - allocated) {
        af_panic("pmm self test: %u allocations consumed %u frames",
                 allocated, before.free_frames - mid.free_frames);
    }

    // Every frame must be distinct. A hint bug that returns the same frame twice
    // is invisible in the counts and catastrophic in use.
    for (af_u32 i = 0; i < allocated; i++) {
        for (af_u32 j = i + 1; j < allocated; j++) {
            if (bulk[i] == bulk[j]) {
                af_panic("pmm self test: frame 0x%lX handed out twice "
                         "(indices %u and %u)", bulk[i], i, j);
            }
        }
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   all %u bulk frames are distinct", allocated);

    for (af_u32 i = 0; i < allocated; i++) {
        pmm_free_frame(bulk[i]);
    }

    pmm_stats(&mid);
    if (mid.free_frames != before.free_frames) {
        af_panic("pmm self test: BULK LEAK — %d frames lost of %u allocated",
                 (int)before.free_frames - (int)mid.free_frames, allocated);
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   bulk alloc/free returns exactly to the start");
}

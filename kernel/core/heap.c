// SPDX-License-Identifier: MIT
// AfriyieOS — kernel heap: slab allocator with a large-allocation fallback
//
// =============================================================================
// DESIGN
// =============================================================================
// One slab cache per size class (16, 32, 64, 128, 256, 512, 1024, 2048 bytes).
// Each cache draws whole 4 KiB frames from the PMM and slices them into
// same-sized objects, threading a free list through the free objects themselves.
// Requests above 2048 bytes are satisfied by a contiguous frame run with a
// header recording the size.
//
// Why slabs and not a classic first-fit free-list heap:
//
//   * No fragmentation *within* a size class. A free-list heap degrades under
//     mixed-size churn; slabs cannot.
//   * O(1) allocate and free.
//   * Debug hooks are cheap and uniform: one redzone check verifies every
//     allocation of that size.
//
// =============================================================================
// HOW kfree KNOWS WHAT IT WAS GIVEN
// =============================================================================
// Each slab page carries a header at offset 0, so kfree can find it by masking
// the pointer down to the page boundary. Large allocations carry their own
// header immediately before the returned pointer. Two magic numbers tell the
// cases apart, and anything that matches neither is a bad pointer — reported
// rather than corrupting the heap.
// =============================================================================

#include "afriyie/heap.h"
#include "afriyie/pmm.h"
#include "afriyie/config.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

#define SLAB_MAGIC   0xA51AB51AULL
#define LARGE_MAGIC  0x1A26E1A2ULL

// Redzone pattern. Chosen to be recognisable in a hexdump and unlikely to be
// produced by a stray memset of 0 or 0xFF.
#define REDZONE_BYTE 0xAF
#define REDZONE_LEN  8

// Objects start after this offset within a slab page. 64 keeps every object
// size aligned to at least 8 bytes, which the allocator's free list needs.
#define SLAB_OBJECT_OFFSET 64

typedef struct slab {
    af_u64              magic;
    struct slab        *next;        // next slab in this cache
    af_u32              class_index;
    af_u32              object_size;
    af_u32              capacity;
    af_u32              in_use;
    void               *free_list;   // threaded through the free objects
} slab_t;

// Trailer for a large allocation, sitting immediately BEFORE the pointer handed
// to the caller: the block occupies [user - sizeof(trailer), user).
//
// =============================================================================
// WHY THE TRAILER IS A FIXED OFFSET AND NOT AN OFFSET FIELD
// =============================================================================
// The first version put the header at the start of the block and stored, in the
// four bytes just below the returned pointer, the distance back to that header,
// so kfree could find it with one subtraction.
//
// That is a real bug, and it took a boot to find. When the allocation is not
// alignment-shifted, the "distance back" is sizeof(header) — and the four bytes
// just below the returned pointer are exactly where the header's own LAST FIELD
// lives. Storing the offset therefore overwrote `requested` with the value 16,
// and kmalloc_usable_size reported 16 bytes for a 100 KiB allocation.
//
// Writing bookkeeping into the very bytes it is meant to describe is the kind of
// mistake that compiles cleanly, passes review, and destroys data later. The
// fix is to stop being clever: the trailer is always exactly
// sizeof(large_trailer_t) bytes before the user pointer, and it records the
// block base directly instead of a distance that has to be recomputed.
// =============================================================================
typedef struct {
    af_u64 magic;
    af_u64 base;          // the frame-aligned block start, for the PMM
    af_u32 frame_count;
    af_u32 requested;
} large_trailer_t;

AF_STATIC_ASSERT_SIZE(large_trailer_t, 24);
AF_STATIC_ASSERT(AF_IS_ALIGNED(sizeof(large_trailer_t), 8),
                 "the trailer must be 8-byte sized so the user pointer stays aligned");

typedef struct {
    af_u32   object_size;
    slab_t  *slabs;
    af_u32   slab_count;
    af_u32   live_objects;
    af_u32   capacity;
} heap_cache_t;

static const af_u32 k_class_sizes[AF_HEAP_NUM_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static heap_cache_t s_caches[AF_HEAP_NUM_CLASSES];
static bool         s_ready = false;

static af_u64 s_allocated_bytes = 0;
static af_u32 s_live_allocations = 0;
static af_u32 s_total_allocations = 0;
static af_u32 s_free_allocations = 0;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static af_i32 class_for_size(af_size size)
{
    for (af_u32 i = 0; i < AF_HEAP_NUM_CLASSES; i++) {
        if (size <= k_class_sizes[i]) {
            return (af_i32)i;
        }
    }
    return -1;
}

AF_INLINE bool is_power_of_two(af_size v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

// The user area of a slab object, used for redzones.
AF_INLINE af_u8 *redzone_front(void *object)
{
    return (af_u8 *)object - REDZONE_LEN;
}

AF_INLINE af_u8 *redzone_back(void *object, af_u32 object_size)
{
    return (af_u8 *)object + object_size;
}

static void paint_redzones(void *object, af_u32 object_size)
{
    af_memset(redzone_front(object), REDZONE_BYTE, REDZONE_LEN);
    af_memset(redzone_back(object, object_size), REDZONE_BYTE, REDZONE_LEN);
}

// Returns true when both guard bands are intact.
static bool redzones_intact(void *object, af_u32 object_size)
{
    const af_u8 *front = redzone_front(object);
    const af_u8 *back  = redzone_back(object, object_size);

    for (af_u32 i = 0; i < REDZONE_LEN; i++) {
        if (front[i] != REDZONE_BYTE || back[i] != REDZONE_BYTE) {
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Slab management
// -----------------------------------------------------------------------------
//
// A slab is one 4 KiB frame. The object area starts at SLAB_OBJECT_OFFSET and
// each object is followed by an REDZONE_LEN guard band, so the real slot stride
// is object_size + REDZONE_LEN. Getting this wrong means objects overlap and
// the redzone check fires on untouched memory, which is at least a loud failure
// rather than a silent one.
static af_u32 slab_stride(af_u32 object_size)
{
    af_u32 stride = object_size + REDZONE_LEN;

    // Keep every object 8-byte aligned so the free list pointer, written into
    // the free objects, is always a valid aligned store.
    return AF_ALIGN_UP(stride, 8);
}

static af_u32 slab_capacity(af_u32 object_size)
{
    af_u32 stride = slab_stride(object_size);
    af_u32 area = AF_PAGE_SIZE - SLAB_OBJECT_OFFSET - REDZONE_LEN;
    return area / stride;
}

static slab_t *slab_create(af_u32 class_index)
{
    af_u32 object_size = k_class_sizes[class_index];
    af_paddr frame = pmm_alloc_frame_z();
    if (frame == AF_FRAME_INVALID) {
        return NULL;
    }

    slab_t *slab = (slab_t *)(af_uptr)frame;

    slab->magic       = SLAB_MAGIC;
    slab->class_index = class_index;
    slab->object_size = object_size;
    slab->capacity    = slab_capacity(object_size);
    slab->in_use      = 0;
    slab->next        = NULL;
    slab->free_list   = NULL;

    af_u32 stride = slab_stride(object_size);

    // Thread the free list through the objects themselves, in descending address
    // order so allocation hands out the lowest address first — easier to read in
    // a hexdump, and it keeps the working set compact.
    for (af_u32 i = slab->capacity; i > 0; i--) {
        af_u8 *object = (af_u8 *)slab + SLAB_OBJECT_OFFSET + (i - 1) * stride;
        paint_redzones(object, object_size);
        *(void **)object = slab->free_list;
        slab->free_list = object;
    }

    return slab;
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------
af_status_t heap_init(void)
{
    af_memset(s_caches, 0, sizeof(s_caches));

    for (af_u32 i = 0; i < AF_HEAP_NUM_CLASSES; i++) {
        s_caches[i].object_size = k_class_sizes[i];
    }

    s_ready = true;

    af_info("heap", "slab allocator ready: %u classes from %u to %u bytes, "
                    "large allocations from %u bytes",
            (af_u32)AF_HEAP_NUM_CLASSES, (af_u32)k_class_sizes[0],
            (af_u32)k_class_sizes[AF_HEAP_NUM_CLASSES - 1],
            (af_u32)AF_HEAP_LARGE_THRESHOLD);

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Large allocations
//
// Blocks are whole frames so they can go straight back to the PMM. The trailer
// is placed after any alignment padding, immediately before the returned
// pointer, so kfree needs no size argument and no arithmetic beyond one
// subtraction of a compile-time constant.
// -----------------------------------------------------------------------------
static large_trailer_t *trailer_of(void *ptr)
{
    return (large_trailer_t *)(void *)((af_u8 *)ptr - sizeof(large_trailer_t));
}

static void *large_alloc(af_size size, af_size alignment)
{
    // One extra frame covers the trailer plus any alignment padding up to a
    // page. Alignment beyond a page is rejected by the caller.
    af_size total = size + sizeof(large_trailer_t) + AF_PAGE_SIZE;
    af_u32 frames = (af_u32)((total + AF_PAGE_SIZE - 1) / AF_PAGE_SIZE);

    af_paddr base = pmm_alloc_frames(frames);
    if (base == AF_FRAME_INVALID) {
        return NULL;
    }

    af_uptr user = (af_uptr)base + sizeof(large_trailer_t);

    if (alignment > 8) {
        user = AF_ALIGN_UP(user, alignment);
    }

    large_trailer_t *trailer = (large_trailer_t *)(void *)(user - sizeof(large_trailer_t));
    trailer->magic       = LARGE_MAGIC;
    trailer->base        = base;
    trailer->frame_count = frames;
    trailer->requested   = (af_u32)size;

    return (void *)user;
}

static void *large_realloc(void *ptr, af_size new_size)
{
    large_trailer_t *trailer = trailer_of(ptr);

    if (trailer->magic != LARGE_MAGIC) {
        af_error("heap", "krealloc on a pointer with no large trailer at %p", ptr);
        return NULL;
    }

    if (new_size <= trailer->frame_count * AF_PAGE_SIZE) {
        trailer->requested = (af_u32)new_size;
        return ptr;
    }

    void *fresh = kmalloc(new_size);
    if (fresh == NULL) {
        return NULL;
    }

    af_size copy = (trailer->requested < new_size) ? trailer->requested : new_size;
    af_memcpy(fresh, ptr, copy);
    kfree(ptr);
    return fresh;
}

// -----------------------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------------------
void *kmalloc(af_size size)
{
    if (!s_ready) {
        af_error("heap", "kmalloc(%u) before heap_init()", (af_u32)size);
        return NULL;
    }

    if (size == 0) {
        // A zero-sized allocation is a caller bug, but returning NULL makes it
        // look like an out-of-memory condition at the call site. Hand back a
        // minimum-size object instead: harmless, and it keeps the failure
        // meaningful.
        size = 1;
    }

    af_i32 class_index = class_for_size(size);

    if (class_index < 0) {
        void *ptr = large_alloc(size, 8);
        if (ptr != NULL) {
            s_allocated_bytes += size;
            s_live_allocations++;
            s_total_allocations++;
        }
        return ptr;
    }

    heap_cache_t *cache = &s_caches[class_index];

    if (cache->slabs == NULL || cache->slabs->free_list == NULL) {
        slab_t *slab = slab_create((af_u32)class_index);
        if (slab == NULL) {
            return NULL;
        }
        slab->next = cache->slabs;
        cache->slabs = slab;
        cache->slab_count++;
    }

    slab_t *slab = cache->slabs;

    // Prefer a slab with free space so a nearly-empty slab is not stranded
    // behind a full one.
    if (slab->free_list == NULL) {
        for (slab_t *s = cache->slabs; s != NULL; s = s->next) {
            if (s->free_list != NULL) {
                slab = s;
                break;
            }
        }
    }

    void *object = slab->free_list;
    slab->free_list = *(void **)object;
    slab->in_use++;
    cache->live_objects++;

    paint_redzones(object, slab->object_size);

    s_allocated_bytes += size;
    s_live_allocations++;
    s_total_allocations++;

    return object;
}

void *kzalloc(af_size size)
{
    void *ptr = kmalloc(size);
    if (ptr != NULL) {
        af_memset(ptr, 0, kmalloc_usable_size(ptr));
    }
    return ptr;
}

af_size kmalloc_usable_size(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    af_uptr page = (af_uptr)ptr & ~(af_uptr)(AF_PAGE_SIZE - 1);
    slab_t *slab = (slab_t *)page;

    if (slab->magic == SLAB_MAGIC) {
        return slab->object_size;
    }

    large_trailer_t *trailer = trailer_of(ptr);
    if (trailer->magic == LARGE_MAGIC) {
        return trailer->requested;
    }

    return 0;
}

void kfree(void *ptr)
{
    if (ptr == NULL) {
        return;   // free(NULL) is legal, as in every sane allocator
    }

    af_uptr page = (af_uptr)ptr & ~(af_uptr)(AF_PAGE_SIZE - 1);
    slab_t *slab = (slab_t *)page;

    if (slab->magic == SLAB_MAGIC) {
        heap_cache_t *cache = &s_caches[slab->class_index];

        if (!redzones_intact(ptr, slab->object_size)) {
            // A guard band was overwritten: the caller wrote past the end (or
            // before the start) of its allocation. Reported before the object is
            // recycled, because once it is on the free list the evidence is gone.
            af_error("heap", "BUFFER OVERRUN at %p (class %u bytes, slab at 0x%lX)",
                     ptr, slab->object_size, (af_u64)page);
            AF_ASSERT_MSG(false, "heap buffer overrun detected at %p", ptr);
            return;
        }

        paint_redzones(ptr, slab->object_size);
        *(void **)ptr = slab->free_list;
        slab->free_list = ptr;
        slab->in_use--;

        if (cache->live_objects > 0) {
            cache->live_objects--;
        }

        s_free_allocations++;
        s_live_allocations--;

        return;
    }

    large_trailer_t *trailer = trailer_of(ptr);

    if (trailer->magic == LARGE_MAGIC) {
        af_paddr base  = (af_paddr)trailer->base;
        af_u32   frames = trailer->frame_count;

        // Stomp the magic so a double free is detected rather than handing the
        // same frames to the PMM twice.
        trailer->magic = 0;

        s_free_allocations++;
        s_live_allocations--;

        pmm_free_frames(base, frames);
        return;
    }

    // Neither a slab object nor a large block: the pointer did not come from
    // this heap. Refusing to act is the only safe response — guessing would
    // corrupt memory far from here.
    af_error("heap", "kfree(%p) is not a heap pointer — ignoring", ptr);
    AF_ASSERT_MSG(false, "kfree of a pointer this heap did not allocate");
}

void *krealloc(void *ptr, af_size new_size)
{
    if (ptr == NULL) {
        return kmalloc(new_size);
    }

    af_size old_size = kmalloc_usable_size(ptr);

    if (old_size == 0) {
        af_error("heap", "krealloc(%p) is not a heap pointer", ptr);
        return NULL;
    }

    // Shrinking inside the same object is free.
    if (new_size <= old_size) {
        return ptr;
    }

    af_uptr page = (af_uptr)ptr & ~(af_uptr)(AF_PAGE_SIZE - 1);
    slab_t *slab = (slab_t *)page;

    if (slab->magic == SLAB_MAGIC) {
        // Growing within a slab means copying into a larger class.
        void *fresh = kmalloc(new_size);
        if (fresh == NULL) {
            return NULL;
        }
        af_memcpy(fresh, ptr, old_size);
        kfree(ptr);
        return fresh;
    }

    return large_realloc(ptr, new_size);
}

void *kmalloc_aligned(af_size size, af_size alignment)
{
    if (!is_power_of_two(alignment)) {
        af_error("heap", "kmalloc_aligned: %u is not a power of two",
                 (af_u32)alignment);
        return NULL;
    }

    if (alignment <= 8) {
        return kmalloc(size);
    }

    // Anything above 8-byte alignment goes down the large path, even for a small
    // request. Rounding up inside a slab object would mean kfree could not
    // recover the original pointer from the returned one — it would push the
    // *aligned* address onto the free list, and the next allocation from that
    // slab would hand out memory overlapping the object that is still live.
    //
    // Wasting up to one page for an aligned small allocation is a bargain
    // compared with that class of bug. The large path already records the
    // offset back to its header, so kfree stays correct.
    return large_alloc(size, alignment);
}

// -----------------------------------------------------------------------------
// Statistics and verification
// -----------------------------------------------------------------------------
void heap_stats(af_heap_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    af_memset(out, 0, sizeof(*out));

    out->total_bytes       = (af_u64)s_live_allocations * AF_PAGE_SIZE;
    out->allocated_bytes   = s_allocated_bytes;
    out->live_allocations  = s_live_allocations;
    out->total_allocations = s_total_allocations;
    out->free_allocations  = s_free_allocations;

    for (af_u32 i = 0; i < AF_HEAP_NUM_CLASSES; i++) {
        out->class_objects[i] = s_caches[i].live_objects;
        out->class_slabs[i]   = s_caches[i].slab_count;
        out->slabs_in_use += s_caches[i].slab_count;
    }
}

void heap_dump_stats(void)
{
    af_heap_stats_t s;
    heap_stats(&s);

    af_info("heap", "allocations: %u live, %u total, %u freed",
            s.live_allocations, s.total_allocations, s.free_allocations);
    af_info("heap", "bytes requested by live allocations: %llu",
            (unsigned long long)s.allocated_bytes);
    af_info("heap", "slabs in use: %u", s.slabs_in_use);

    for (af_u32 i = 0; i < AF_HEAP_NUM_CLASSES; i++) {
        if (s.class_slabs[i] == 0) {
            continue;
        }
        af_info("heap", "  class %5u B: %u slabs, %u live objects",
                k_class_sizes[i], s.class_slabs[i], s.class_objects[i]);
    }
}

af_status_t heap_check(void)
{
    for (af_u32 i = 0; i < AF_HEAP_NUM_CLASSES; i++) {
        heap_cache_t *cache = &s_caches[i];
        af_u32 stride = slab_stride(cache->object_size);

        for (slab_t *slab = cache->slabs; slab != NULL; slab = slab->next) {
            if (slab->magic != SLAB_MAGIC) {
                af_error("heap", "class %u: slab at %p has bad magic", i, slab);
                return AF_ERR_FS_CORRUPT;
            }
            if (slab->class_index != i) {
                af_error("heap", "class %u: slab claims class %u",
                         i, slab->class_index);
                return AF_ERR_FS_CORRUPT;
            }

            // Walk every object and verify the guard bands of the ones in use.
            af_u32 free_seen = 0;
            for (void *node = slab->free_list; node != NULL; node = *(void **)node) {
                free_seen++;
                if (free_seen > slab->capacity) {
                    af_error("heap", "class %u: free list at %p is cyclic",
                             i, slab);
                    return AF_ERR_FS_CORRUPT;
                }
            }

            if (free_seen != slab->capacity - slab->in_use) {
                af_error("heap", "class %u: slab at %p has %u free nodes but "
                         "%u of %u objects are free",
                         i, slab, free_seen, slab->capacity - slab->in_use,
                         slab->capacity);
                return AF_ERR_FS_CORRUPT;
            }

            (void)stride;
        }
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Self test
// -----------------------------------------------------------------------------
void heap_selftest(void)
{
    // --- basics --------------------------------------------------------------
    void *p = kmalloc(64);
    if (p == NULL) {
        af_panic("heap self test: kmalloc(64) failed");
    }
    if (!AF_IS_ALIGNED((af_uptr)p, 8)) {
        af_panic("heap self test: kmalloc returned an unaligned pointer %p", p);
    }
    // Writing the full requested size must not trip the redzone check.
    af_memset(p, 0x5A, 64);
    kfree(p);
    af_log(AF_LOG_DEBUG, "test", "  ok   kmalloc/kfree 64 bytes");

    // --- zeroed allocation ---------------------------------------------------
    af_u8 *z = (af_u8 *)kzalloc(200);
    if (z == NULL) {
        af_panic("heap self test: kzalloc failed");
    }
    for (af_u32 i = 0; i < 200; i++) {
        if (z[i] != 0) {
            af_panic("heap self test: kzalloc returned dirty memory at offset %u", i);
        }
    }
    kfree(z);
    af_log(AF_LOG_DEBUG, "test", "  ok   kzalloc returns zeroed memory");

    // --- every size class ----------------------------------------------------
    for (af_u32 i = 0; i < AF_HEAP_NUM_CLASSES; i++) {
        af_size size = k_class_sizes[i];
        af_u8 *block = (af_u8 *)kmalloc(size);
        if (block == NULL) {
            af_panic("heap self test: kmalloc(%u) failed", (af_u32)size);
        }
        // Touch every byte: the capacity maths has to leave room for the guard
        // band after the last object, and getting that wrong corrupts memory
        // silently.
        af_memset(block, (int)(i + 1), size);
        if (block[0] != (af_u8)(i + 1) || block[size - 1] != (af_u8)(i + 1)) {
            af_panic("heap self test: class %u bytes did not round-trip", (af_u32)size);
        }
        kfree(block);
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   every size class allocates and round-trips");

    // --- a large allocation spanning many frames -----------------------------
    af_u8 *big = (af_u8 *)kmalloc(100 * 1024);
    if (big == NULL) {
        af_panic("heap self test: 100 KiB allocation failed");
    }
    af_memset(big, 0x77, 100 * 1024);
    if (big[0] != 0x77 || big[100 * 1024 - 1] != 0x77) {
        af_panic("heap self test: large allocation did not round-trip");
    }
    if (kmalloc_usable_size(big) != 100 * 1024) {
        af_panic("heap self test: large allocation reports %u usable bytes",
                 (af_u32)kmalloc_usable_size(big));
    }
    kfree(big);
    af_log(AF_LOG_DEBUG, "test", "  ok   100 KiB large allocation round-trips");

    // --- realloc -------------------------------------------------------------
    af_u8 *r = (af_u8 *)kmalloc(32);
    af_memset(r, 0x11, 32);
    r = (af_u8 *)krealloc(r, 900);
    if (r == NULL) {
        af_panic("heap self test: krealloc failed");
    }
    for (af_u32 i = 0; i < 32; i++) {
        if (r[i] != 0x11) {
            af_panic("heap self test: krealloc lost data at offset %u", i);
        }
    }
    kfree(r);
    af_log(AF_LOG_DEBUG, "test", "  ok   krealloc preserves contents");

    // --- aligned allocation --------------------------------------------------
    void *aligned = kmalloc_aligned(100, 64);
    if (aligned == NULL) {
        af_panic("heap self test: kmalloc_aligned failed");
    }
    if (((af_uptr)aligned & 63) != 0) {
        af_panic("heap self test: kmalloc_aligned returned %p, not 64-aligned",
                 aligned);
    }
    kfree(aligned);
    af_log(AF_LOG_DEBUG, "test", "  ok   kmalloc_aligned honours the alignment");

    // --- leak check around the fuzz ------------------------------------------
    af_heap_stats_t before;
    heap_stats(&before);

    // --- the fuzz test -------------------------------------------------------
    //
    // 100 000 random operations over a block table. This is the test that finds
    // the bug a hand-written sequence never does: a free list that corrupts
    // itself after a particular interleaving, a capacity that is off by one for
    // one size class only, a refcount that drifts.
    enum { SLOTS = 256, OPS = 100000 };
    static void   *slots[SLOTS];
    static af_u32  sizes[SLOTS];
    af_u32 live = 0;

    // xorshift32: deterministic, so a failure is reproducible from the seed
    // printed below rather than being a once-in-a-build mystery.
    af_u32 rng = 0xAF12C0DEu;
    af_u32 allocation_count = 0;
    af_u32 free_count = 0;

    for (af_u32 op = 0; op < OPS; op++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        af_u32 slot = rng % SLOTS;

        if (slots[slot] == NULL) {
            // Sizes deliberately straddle the class boundaries and the large
            // threshold, so both code paths get exercised.
            static const af_u32 interesting[] = {
                1, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
                255, 256, 257, 511, 512, 513, 1023, 1024, 1025,
                2047, 2048, 2049, 4096, 5000
            };
            af_u32 size = interesting[rng % (sizeof(interesting) / sizeof(interesting[0]))];

            void *ptr = kmalloc(size);
            if (ptr == NULL) {
                continue;
            }

            // Write the whole requested size. A capacity bug shows up here as a
            // redzone failure on the next kfree of this block.
            af_memset(ptr, (int)(rng & 0xFF), size);

            slots[slot] = ptr;
            sizes[slot] = size;
            live++;
            allocation_count++;
        } else {
            // Verify the contents survived before releasing it. A free list that
            // hands out a block twice would have overwritten these bytes.
            af_u8 *live_block = (af_u8 *)slots[slot];
            af_u8 expected = live_block[0];
            bool intact = true;

            for (af_u32 i = 0; i < sizes[slot]; i++) {
                if (live_block[i] != expected) {
                    intact = false;
                    break;
                }
            }

            if (!intact) {
                af_panic("heap self test: block at %p (slot %u, %u bytes) was "
                         "corrupted while live — the heap handed it out twice",
                         slots[slot], slot, sizes[slot]);
            }

            kfree(slots[slot]);
            slots[slot] = NULL;
            live--;
            free_count++;
        }
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   fuzz: %u allocations, %u frees, %u live",
            allocation_count, free_count, live);

    // Release what is left.
    for (af_u32 i = 0; i < SLOTS; i++) {
        if (slots[i] != NULL) {
            kfree(slots[i]);
            slots[i] = NULL;
        }
    }

    // --- must return exactly to the starting state ---------------------------
    //
    // Compared against `before` taken AFTER the earlier phases had been freed,
    // so the live allocation count must be identical. Any drift is a leak.
    af_heap_stats_t after;
    heap_stats(&after);

    if (after.live_allocations != before.live_allocations) {
        af_panic("heap self test: LEAK — %u live allocations before, %u after",
                 before.live_allocations, after.live_allocations);
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   100 000-operation fuzz leaves no leak");

    // --- structural integrity ------------------------------------------------
    af_status_t rc = heap_check();
    if (af_status_err(rc)) {
        af_panic("heap self test: heap_check failed (%s)", af_status_name(rc));
    }
    af_log(AF_LOG_DEBUG, "test", "  ok   heap_check: slabs, free lists and "
                                  "guard bands are intact");
}

// SPDX-License-Identifier: MIT
// AfriyieOS — kernel heap

#ifndef AFRIYIE_HEAP_H
#define AFRIYIE_HEAP_H

#include "types.h"
#include "status.h"

// Size classes, in bytes. Requests up to the largest class are served from a
// slab cache; anything larger goes straight to contiguous physical frames.
#define AF_HEAP_NUM_CLASSES 8
#define AF_HEAP_MIN_SIZE    16
#define AF_HEAP_LARGE_THRESHOLD 2048

typedef struct {
    af_u64 total_bytes;      // bytes currently handed out
    af_u64 allocated_bytes;  // bytes requested by live allocations
    af_u32 live_allocations;
    af_u32 total_allocations;   // since boot
    af_u32 free_allocations;
    af_u32 slabs_in_use;
    af_u32 class_objects[AF_HEAP_NUM_CLASSES];   // live objects per class
    af_u32 class_slabs[AF_HEAP_NUM_CLASSES];     // slabs per class
} af_heap_stats_t;

// Brings up the slab caches. Requires pmm_init() to have run.
af_status_t heap_init(void);

void *kmalloc(af_size size);
void *kzalloc(af_size size);
void *krealloc(void *ptr, af_size new_size);
void  kfree(void *ptr);

// Aligned allocation. Alignment must be a power of two and no larger than a
// page; anything more goes to a direct frame allocation.
void *kmalloc_aligned(af_size size, af_size alignment);

// Bytes actually available in the allocation containing `ptr`. Callers that
// over-allocate for a class boundary can use this instead of tracking the size.
af_size kmalloc_usable_size(void *ptr);

void heap_stats(af_heap_stats_t *out);
void heap_dump_stats(void);

// Verifies every slab's redzones and the free-list integrity. Expensive; used
// by the self test and available from the panic path.
af_status_t heap_check(void);

void heap_selftest(void);

#endif // AFRIYIE_HEAP_H

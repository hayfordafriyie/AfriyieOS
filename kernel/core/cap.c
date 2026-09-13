// SPDX-License-Identifier: MIT
// AfriyieOS — capability tables
//
// The implementation of the two rules in cap.h. There is very little code here
// and that is the point: authority is decided in ONE function (cap_lookup's
// rights test) and narrowed in ONE place (cap_derive's subset test). A rights
// system with two places that decide is a rights system with a bypass.

#include "afriyie/cap.h"
#include "afriyie/heap.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

// -----------------------------------------------------------------------------
// The table
// -----------------------------------------------------------------------------
typedef struct {
    af_object_t object;
    af_u32      rights;
    af_u32      generation;

    // The slot this capability was derived from, or AF_CAP_NO_PARENT. This one
    // field is what makes revocation transitive: revoking a capability walks the
    // table for slots naming it as a parent, recurses into those, and so on.
    //
    // A parent INDEX rather than a handle, because the parent's generation is
    // irrelevant to the relationship — if the parent slot is reused for a
    // different capability, the children should not follow it. Comparing the
    // index alone means a child of a since-reused slot is simply never reached
    // by a revoke, which is correct: its parent is gone, so it is already
    // orphaned and revoking it separately is the only way to disarm it.
    af_u16      parent;

    bool        used;
} af_cap_slot_t;

struct af_cap_table {
    af_cap_slot_t slots[AF_CAP_MAX_SLOTS];
    af_u32        used;
};

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
af_cap_table_t *cap_table_create(void)
{
    af_cap_table_t *table = (af_cap_table_t *)kzalloc(sizeof(af_cap_table_t));
    if (table == NULL) {
        af_error("cap", "could not allocate a capability table (%u bytes)",
                 (af_u32)sizeof(af_cap_table_t));
        return NULL;
    }

    // Generations start at 1 so that a handle built from a fresh slot is
    // non-zero, and so that "generation 0" never appears in the packed handle
    // where it could be confused with the invalid value.
    for (af_u32 i = 0; i < AF_CAP_MAX_SLOTS; i++) {
        table->slots[i].generation = 1;
        table->slots[i].parent     = AF_CAP_NO_PARENT;
    }

    return table;
}

void cap_table_destroy(af_cap_table_t *table)
{
    // Nothing here frees the OBJECTS. A capability is a reference to something
    // owned elsewhere, and destroying the references must not destroy the
    // referents — a frame two processes share would be freed by whichever of
    // them exited first. Frame ownership is the address space's business, and it
    // is handled by hal_pt_destroy's reference counting.
    kfree(table);
}

// -----------------------------------------------------------------------------
// Handle packing
//
// index in the low 8 bits, generation above it. AF_CAP_MAX_SLOTS is 256, so the
// index always fits and the split cannot drift — a static assert in the header
// would be better than this comment, and is worth adding when the slot count
// next changes.
// -----------------------------------------------------------------------------
static af_cap_t pack(af_u32 index, af_u32 generation)
{
    return (af_cap_t)((generation << AF_CAP_INDEX_BITS) | (index & AF_CAP_INDEX_MASK));
}

static af_u32 unpack_index(af_cap_t cap)
{
    return (af_u32)(cap & AF_CAP_INDEX_MASK);
}

static af_u32 unpack_generation(af_cap_t cap)
{
    return (af_u32)(cap >> AF_CAP_INDEX_BITS);
}

// Resolves a handle to a slot, or NULL. Checks the generation — this is the
// function that makes a stale handle fail rather than address a recycled slot.
static af_cap_slot_t *resolve(af_cap_table_t *table, af_cap_t cap)
{
    if (table == NULL || cap == AF_CAP_INVALID) {
        return NULL;
    }

    const af_u32 index = unpack_index(cap);
    if (index == 0 || index >= AF_CAP_MAX_SLOTS) {
        return NULL;
    }

    af_cap_slot_t *slot = &table->slots[index];

    if (!slot->used) {
        return NULL;
    }

    // THE GENERATION CHECK. Without it, a handle to a freed slot silently
    // addresses whatever was allocated into it next.
    if (slot->generation != unpack_generation(cap)) {
        return NULL;
    }

    return slot;
}

// -----------------------------------------------------------------------------
// Insert
// -----------------------------------------------------------------------------
af_cap_t cap_insert(af_cap_table_t *table, af_object_t object, af_u32 rights)
{
    if (table == NULL || object.type == AF_OBJECT_NONE) {
        return AF_CAP_INVALID;
    }

    // Slot 0 is never handed out, so that AF_CAP_INVALID (which is 0) can never
    // collide with a real capability. Searching from 1 rather than skipping is
    // deliberate: a "reserved" slot that is never allocated is simpler to reason
    // about than one that is allocated and then special-cased everywhere.
    for (af_u32 i = 1; i < AF_CAP_MAX_SLOTS; i++) {
        af_cap_slot_t *slot = &table->slots[i];
        if (slot->used) {
            continue;
        }

        slot->object = object;
        slot->rights = rights;
        slot->parent = AF_CAP_NO_PARENT;
        slot->used   = true;

        table->used++;

        return pack(i, slot->generation);
    }

    af_error("cap", "capability table is full (%u slots)", AF_CAP_MAX_SLOTS);
    return AF_CAP_INVALID;
}

// -----------------------------------------------------------------------------
// Lookup — the only place authority is decided
// -----------------------------------------------------------------------------
af_status_t cap_lookup(af_cap_table_t *table, af_cap_t cap, af_u32 required,
                       af_object_t *out)
{
    af_cap_slot_t *slot = resolve(table, cap);
    if (slot == NULL) {
        return AF_ERR_CAP_INVALID;
    }

    // The subset test. `required` must be fully covered by what the capability
    // carries; a single missing right is a denial. Written as a single
    // expression with no early special cases, because a special case here is a
    // bypass.
    if ((slot->rights & required) != required) {
        return AF_ERR_PERM;
    }

    if (out != NULL) {
        *out = slot->object;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Derive — Rule 1
// -----------------------------------------------------------------------------
af_status_t cap_derive(af_cap_table_t *table, af_cap_t cap, af_u32 rights,
                       af_cap_t *out)
{
    af_cap_slot_t *parent = resolve(table, cap);
    if (parent == NULL) {
        return AF_ERR_CAP_INVALID;
    }

    if (out == NULL) {
        return AF_ERR_INVAL;
    }

    // RULE 1. This one line is the difference between a capability system and a
    // set of integers. Everything else here would still work if it were removed;
    // what would be lost is the guarantee that authority only ever decreases.
    //
    // Note the direction of the test: `(rights & ~parent->rights) != 0` would
    // also be correct, but the form below states the positive case and reads as
    // the rule rather than as its negation.
    if ((rights & parent->rights) != rights) {
        af_warn("cap", "derive refused: requested rights 0x%X are not a subset "
                       "of the held 0x%X", rights, parent->rights);
        return AF_ERR_PERM;
    }

    const af_cap_t child = cap_insert(table, parent->object, rights);
    if (child == AF_CAP_INVALID) {
        return AF_ERR_TOOMANY;
    }

    // Record the parent so revoke can find this slot. AF_CAP_INVALID from
    // cap_insert means the insert failed, so the resolve below cannot fail —
    // but it is checked rather than assumed, because an assumption that a
    // just-inserted handle resolves is exactly the kind that stops being true.
    af_cap_slot_t *child_slot = resolve(table, child);
    if (child_slot != NULL) {
        child_slot->parent = (af_u16)unpack_index(cap);
    }

    *out = child;
    return AF_OK;
}

// -----------------------------------------------------------------------------
// Delete
// -----------------------------------------------------------------------------
af_status_t cap_delete(af_cap_table_t *table, af_cap_t cap)
{
    af_cap_slot_t *slot = resolve(table, cap);
    if (slot == NULL) {
        return AF_ERR_CAP_INVALID;
    }

    slot->used = false;
    slot->object.type = AF_OBJECT_NONE;
    slot->object.id   = 0;
    slot->rights      = 0;
    slot->parent      = AF_CAP_NO_PARENT;

    // Bumping the generation on FREE rather than on reallocation is what makes
    // the stale handle fail. Doing it on reuse would work too, but only if every
    // allocation path remembered to — and this way there is no allocation path
    // that can forget.
    slot->generation++;

    if (table->used > 0) {
        table->used--;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Revoke — Rule 2
//
// Recursive rather than iterative-with-a-stack, and bounded by the table size:
// the derivation tree can be at most as deep as there are slots, and each
// recursion consumes one slot permanently (a deleted slot is not revisited
// because resolve() fails on it afterwards). So the depth is bounded by
// AF_CAP_MAX_SLOTS and there is no stack overflow risk — which is the objection
// that normally rules out recursion in a kernel.
// -----------------------------------------------------------------------------
static af_u32 revoke_children(af_cap_table_t *table, af_u32 parent_index)
{
    af_u32 revoked = 0;

    for (af_u32 i = 1; i < AF_CAP_MAX_SLOTS; i++) {
        af_cap_slot_t *slot = &table->slots[i];

        if (!slot->used || slot->parent != (af_u16)parent_index) {
            continue;
        }

        // Depth first, so a grandchild is gone before its parent. The order does
        // not matter for correctness — every descendant is reached either way —
        // but it means a partially-completed revoke leaves a shallower tree
        // rather than orphaning children from a deleted parent.
        revoked += revoke_children(table, i);

        // Delete by handle so the generation is bumped and the slot is released
        // by exactly the same code path as an ordinary delete. A revoke that
        // freed slots its own way would be a second implementation of delete,
        // and the generation handling is precisely the part that must not have a
        // second implementation.
        cap_delete(table, pack(i, slot->generation));
        revoked++;
    }

    return revoked;
}

af_status_t cap_revoke(af_cap_table_t *table, af_cap_t cap)
{
    af_cap_slot_t *slot = resolve(table, cap);
    if (slot == NULL) {
        return AF_ERR_CAP_INVALID;
    }

    // A capability without AF_RIGHT_REVOKE may stop using the object but may not
    // disarm anyone else. This is what stops a borrowed read-only handle from
    // being used to tear down the owner's derived capabilities.
    if ((slot->rights & AF_RIGHT_REVOKE) == 0) {
        af_warn("cap", "revoke refused: capability lacks AF_RIGHT_REVOKE");
        return AF_ERR_PERM;
    }

    const af_u32 index    = unpack_index(cap);
    const af_u32 children = revoke_children(table, index);

    cap_delete(table, cap);

    af_info("cap", "revoked a capability and %u derived from it", children);
    return AF_OK;
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
af_u32 cap_used_slots(const af_cap_table_t *table)
{
    return (table != NULL) ? table->used : 0;
}

af_u32 cap_generation_of(const af_cap_table_t *table, af_cap_t cap)
{
    if (table == NULL) {
        return 0;
    }
    const af_u32 index = unpack_index(cap);
    if (index == 0 || index >= AF_CAP_MAX_SLOTS) {
        return 0;
    }
    return table->slots[index].generation;
}

const char *cap_object_type_name(af_object_type_t type)
{
    switch (type) {
    case AF_OBJECT_NONE:         return "none";
    case AF_OBJECT_FRAME:        return "frame";
    case AF_OBJECT_ENDPOINT:     return "endpoint";
    case AF_OBJECT_NOTIFICATION: return "notification";
    case AF_OBJECT_FILE:         return "file";
    case AF_OBJECT_DEVICE:       return "device";
    case AF_OBJECT_IRQ:          return "irq";
    case AF_OBJECT_PROCESS:      return "process";
    }
    return "?";
}

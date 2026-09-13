// SPDX-License-Identifier: MIT
// AfriyieOS — capabilities
//
// A capability is an unforgeable reference to an object, carrying the rights its
// holder may exercise over it. It is the only form of authority in AfriyieOS:
// there is no ambient privilege, no uid, and no path that a process may simply
// name. See docs/architecture/capability-model.md.
//
// WHY THIS EXISTS NOW, AND DID NOT BEFORE
//
// A capability is held by a PROCESS. Until v0.6 there was no process object, so
// there was nowhere to put a table and no answer to "who holds this?". The field
// was deliberately left out rather than stubbed, because a table that nothing
// enforces looks exactly like isolation and is not.
//
// This is also what every foreign personality runs on (ADR-012). A Linux binary
// translated into our syscalls must have its `open()` become a capability
// lookup, and a Windows `CreateFile` likewise — and when a translator has a bug,
// the worst it can do is misuse a capability the user granted. It cannot name a
// file the process was never given, because in this system there is no way to
// name one.
//
// THE TWO RULES, and everything here follows from them:
//
//   Rule 1 — derivation may only NARROW. cap_derive succeeds only when the
//            requested rights are a subset of those held. There is no operation
//            anywhere in the system that widens a capability.
//
//   Rule 2 — revocation is TRANSITIVE. Revoking a capability destroys everything
//            derived from it, recursively.
//
// Together they give a property a name-based system cannot offer: a process can
// be fully disarmed. Revoke what it was given and everything built on top of it
// disappears, with no way back in, because there was never a way to reach the
// object except through the capability.

#ifndef AFRIYIE_CAP_H
#define AFRIYIE_CAP_H

#include "types.h"
#include "status.h"

struct af_process;

// -----------------------------------------------------------------------------
// Rights
//
// A bitmask rather than an enum because rights compose: a capability to a frame
// may be readable AND writable AND grantable, and the check is a subset test.
// -----------------------------------------------------------------------------
#define AF_RIGHT_READ     (1u << 0)   // read the object's state
#define AF_RIGHT_WRITE    (1u << 1)   // modify it
#define AF_RIGHT_EXEC     (1u << 2)   // execute code mapped from it
#define AF_RIGHT_GRANT    (1u << 3)   // hand this capability to another process
#define AF_RIGHT_REVOKE   (1u << 4)   // revoke capabilities derived from this one
#define AF_RIGHT_SIGNAL   (1u << 5)   // signal it — endpoint send, notification
#define AF_RIGHT_WAIT     (1u << 6)   // block on it — endpoint receive, wait

#define AF_RIGHT_ALL      (AF_RIGHT_READ | AF_RIGHT_WRITE | AF_RIGHT_EXEC | \
                           AF_RIGHT_GRANT | AF_RIGHT_REVOKE | AF_RIGHT_SIGNAL | \
                           AF_RIGHT_WAIT)

// -----------------------------------------------------------------------------
// Objects
//
// A tagged reference. Deliberately not a pointer: a capability must not become a
// way to reach kernel memory, and an opaque type/id pair cannot be dereferenced
// by accident. The type is what a rights check is meaningful against — READ on a
// frame and READ on an endpoint are different operations, and collapsing them to
// one "read" bit is how a rights system becomes decorative.
// -----------------------------------------------------------------------------
typedef enum {
    AF_OBJECT_NONE = 0,
    AF_OBJECT_FRAME,          // id = physical address of the frame
    AF_OBJECT_ENDPOINT,       // id = endpoint id (v0.7)
    AF_OBJECT_NOTIFICATION,   // id = notification id (v0.8)
    AF_OBJECT_FILE,           // id = file id in the AFS store (v0.9)
    AF_OBJECT_DEVICE,         // id = device id
    AF_OBJECT_IRQ,            // id = IRQ line
    AF_OBJECT_PROCESS,        // id = pid
} af_object_type_t;

typedef struct {
    af_object_type_t type;
    af_u64           id;
} af_object_t;

// -----------------------------------------------------------------------------
// The handle
//
// A slot index AND a generation, packed into one word. The generation is what
// stops a stale handle from reaching a different object:
//
//   process hands out handle H for slot 7
//   slot 7 is freed
//   slot 7 is reallocated for a different frame
//   process presents H again
//
// With only an index, that third step succeeds and the process has silently
// acquired a capability it was never given. With a generation it fails, because
// the slot's generation was incremented when it was reused and H carries the old
// one.
//
// 0 is never valid, so an uninitialised af_cap_t is the invalid case rather than
// something a caller has to remember to set.
// -----------------------------------------------------------------------------
typedef af_u32 af_cap_t;

#define AF_CAP_INVALID       0u
#define AF_CAP_INDEX_MASK    0x000000FFu
#define AF_CAP_INDEX_BITS    8u
#define AF_CAP_MAX_SLOTS     256u
#define AF_CAP_NO_PARENT     0xFFFFu

// -----------------------------------------------------------------------------
// Table lifetime
//
// Allocated from the heap with the process and freed with it, NOT embedded in
// af_process_t. 256 processes x 256 slots x 24 bytes is 1.5 MiB of BSS for a
// table that is mostly empty, and a process that does not exist has no business
// reserving capability slots.
// -----------------------------------------------------------------------------
typedef struct af_cap_table af_cap_table_t;

af_cap_table_t *cap_table_create(void);
void            cap_table_destroy(af_cap_table_t *table);

// -----------------------------------------------------------------------------
// Operations
//
// Every one takes the TABLE rather than deriving it from the current process.
// A syscall passes the caller's table; a future kernel thread acting on behalf
// of a process passes that process's. Deriving it implicitly would mean the
// check depended on who happened to be running, which is exactly the confusion
// capabilities exist to remove.
// -----------------------------------------------------------------------------

// Inserts a capability and returns its handle, or AF_CAP_INVALID when the table
// is full.
af_cap_t cap_insert(af_cap_table_t *table, af_object_t object, af_u32 rights);

// Resolves a handle and checks it carries at least `required` rights.
//
// On success fills `out` with the object. Failure is AF_ERR_CAP_INVALID for a
// stale or empty handle and AF_ERR_PERM for a handle that exists but does not
// carry the rights — DIFFERENT ERRORS ON PURPOSE. Collapsing them would make a
// permissions bug indistinguishable from a use-after-free, and the two have
// nothing in common except a failing test.
af_status_t cap_lookup(af_cap_table_t *table, af_cap_t cap, af_u32 required,
                       af_object_t *out);

// Derives a narrower capability. RULE 1: succeeds only when the requested rights
// are a subset of those held. Asking for `AF_RIGHT_ALL` from a read-only
// capability returns AF_ERR_PERM — this is the check that makes "a process can
// only ever lose authority" true rather than aspirational.
//
// The new slot records its parent, which is what makes revocation transitive.
af_status_t cap_derive(af_cap_table_t *table, af_cap_t cap, af_u32 rights,
                       af_cap_t *out);

// Frees one slot. Does NOT touch capabilities derived from it — that is revoke's
// job, and conflating the two makes "drop this handle" quietly disarm an
// unrelated part of the process.
af_status_t cap_delete(af_cap_table_t *table, af_cap_t cap);

// Rule 2: destroys this capability and everything derived from it, recursively.
//
// Note what this does NOT yet do: unmap frames that were mapped through the
// revoked capability. There is no object-to-mapping record because there is no
// mapping-through-a-capability path yet — the current loader maps directly. That
// registry arrives with the memory objects the IPC work needs, and until then
// this is documented as incomplete rather than claimed as done.
af_status_t cap_revoke(af_cap_table_t *table, af_cap_t cap);

// How many slots are in use, and how many generations have been consumed.
// For diagnostics and for the self test.
af_u32 cap_used_slots(const af_cap_table_t *table);
af_u32 cap_generation_of(const af_cap_table_t *table, af_cap_t cap);

const char *cap_object_type_name(af_object_type_t type);

// Boot-time checks: insert/lookup, rights enforcement, narrowing, stale-handle
// detection, and transitive revocation. Emits AF_CAP_OK.
void af_cap_selftest(void);

#endif // AFRIYIE_CAP_H

// SPDX-License-Identifier: MIT
// AfriyieOS — capability self test
//
// THE FOUR PROPERTIES THAT MATTER, each of which fails silently if it fails at
// all in production:
//
//   1. A capability resolves to the object it was issued for.
//   2. A capability that does not carry a required right is REFUSED, and refused
//      with a different error from a stale handle.
//   3. Derivation cannot widen. This is Rule 1, and it is the single line that
//      separates a capability system from a set of integers.
//   4. Revocation is transitive. This is Rule 2, and it is what makes a process
//      fully disarmable.
//
// Plus the stale-handle case, which is the one a plausible-looking implementation
// gets wrong: free a slot, reallocate it, present the old handle, and see whether
// the system notices that it is addressing a different object.

#include "afriyie/cap.h"
#include "afriyie/log.h"
#include "afriyie/kstring.h"

static af_u32 s_checks;
static af_u32 s_failures;

static void check(bool condition, const char *what)
{
    s_checks++;
    if (!condition) {
        s_failures++;
        af_error("cap", "  FAIL  %s", what);
    }
}

static af_object_t frame_obj(af_u64 id)
{
    af_object_t o;
    o.type = AF_OBJECT_FRAME;
    o.id   = id;
    return o;
}

void af_cap_selftest(void)
{
    s_checks = 0;
    s_failures = 0;

    af_cap_table_t *table = cap_table_create();
    check(table != NULL, "a capability table can be created");
    if (table == NULL) {
        af_error("cap", "cannot continue without a table");
        af_marker("AF_TEST_FAIL");
        return;
    }

    check(cap_used_slots(table) == 0, "a new table is empty");

    // =========================================================================
    // 1. Insert and look up
    // =========================================================================
    af_object_t frame_a = frame_obj(0x1E00000);
    af_cap_t cap_a = cap_insert(table, frame_a, AF_RIGHT_READ | AF_RIGHT_WRITE);

    check(cap_a != AF_CAP_INVALID, "a capability can be inserted");
    check(cap_used_slots(table) == 1, "the used count went up");

    af_object_t got;
    af_status_t rc = cap_lookup(table, cap_a, AF_RIGHT_READ, &got);
    check(rc == AF_OK, "a held right resolves");
    check(got.type == AF_OBJECT_FRAME, "the object type round-trips");
    check(got.id == frame_a.id, "the object id round-trips");

    // =========================================================================
    // 2. Rights enforcement, and the two errors being different
    // =========================================================================
    rc = cap_lookup(table, cap_a, AF_RIGHT_READ | AF_RIGHT_WRITE, &got);
    check(rc == AF_OK, "the full held set resolves");

    rc = cap_lookup(table, cap_a, AF_RIGHT_EXEC, &got);
    check(rc == AF_ERR_PERM, "a right NOT held is refused with AF_ERR_PERM");

    rc = cap_lookup(table, cap_a, AF_RIGHT_READ | AF_RIGHT_EXEC, &got);
    check(rc == AF_ERR_PERM,
          "a partially held set is refused — one missing right is a denial");

    // A stale handle and an insufficient one are different failures and must
    // report differently. Collapsing them makes a permissions bug
    // indistinguishable from a use-after-free, and the two have nothing in
    // common except a failing test.
    rc = cap_lookup(table, AF_CAP_INVALID, AF_RIGHT_READ, &got);
    check(rc == AF_ERR_CAP_INVALID, "the invalid handle is AF_ERR_CAP_INVALID");

    af_cap_t bogus = 0x00000700u;   // generation 7, slot 0 — never allocated
    rc = cap_lookup(table, bogus, AF_RIGHT_READ, &got);
    check(rc == AF_ERR_CAP_INVALID, "a handle to slot 0 is never valid");

    // =========================================================================
    // 3. Derivation may only narrow — Rule 1
    // =========================================================================
    af_cap_t narrowed = AF_CAP_INVALID;
    rc = cap_derive(table, cap_a, AF_RIGHT_READ, &narrowed);
    check(rc == AF_OK, "deriving a SUBSET of the held rights succeeds");
    check(cap_used_slots(table) == 2, "the derived capability took a slot");

    rc = cap_lookup(table, narrowed, AF_RIGHT_READ, &got);
    check(rc == AF_OK, "the derived capability grants what was asked for");
    check(got.id == frame_a.id, "and it names the SAME object");

    rc = cap_lookup(table, narrowed, AF_RIGHT_WRITE, &got);
    check(rc == AF_ERR_PERM,
          "the derived capability does NOT grant the parent's other rights");

    // THE RULE. This is the assertion that would fail if cap_derive forgot its
    // subset test, and everything else in this file would still pass.
    af_cap_t widened = AF_CAP_INVALID;
    rc = cap_derive(table, narrowed, AF_RIGHT_WRITE, &widened);
    check(rc == AF_ERR_PERM, "deriving a WIDER right is refused");
    check(widened == AF_CAP_INVALID, "a refused derive produces no handle");

    rc = cap_derive(table, narrowed, AF_RIGHT_ALL, &widened);
    check(rc == AF_ERR_PERM, "AF_RIGHT_ALL cannot be obtained by derivation");

    // Deriving the SAME rights is allowed — it is a subset of itself, and it is
    // how a process makes a copy to hand to a thread without gaining anything.
    af_cap_t same = AF_CAP_INVALID;
    rc = cap_derive(table, cap_a, AF_RIGHT_READ | AF_RIGHT_WRITE, &same);
    check(rc == AF_OK, "deriving the same rights is allowed");

    // =========================================================================
    // 4. Revocation is transitive — Rule 2
    //
    // The chain is built from a capability that HOLDS AF_RIGHT_REVOKE, because
    // derivation cannot confer a right the parent does not have — that is Rule 1,
    // and the first version of this test tried to derive REVOKE from a
    // read/write capability and was correctly refused. The test was wrong and the
    // code was right, which is the outcome a positive assertion is for.
    //
    // cap_a deliberately does NOT hold REVOKE, so it can be used for the refusal
    // case below.
    // =========================================================================
    af_cap_t root = cap_insert(table, frame_obj(0x1E00000),
                               AF_RIGHT_READ | AF_RIGHT_REVOKE);
    check(root != AF_CAP_INVALID, "a revocable capability was issued");

    const af_u32 before_revoke = cap_used_slots(table);

    af_cap_t chain = AF_CAP_INVALID;
    rc = cap_derive(table, root, AF_RIGHT_READ | AF_RIGHT_REVOKE, &chain);
    check(rc == AF_OK, "a revocable child was derived");

    af_cap_t grandchild = AF_CAP_INVALID;
    rc = cap_derive(table, chain, AF_RIGHT_READ, &grandchild);
    check(rc == AF_OK, "a grandchild was derived");

    check(cap_used_slots(table) == before_revoke + 2, "the chain took two slots");

    // cap_a does not carry AF_RIGHT_REVOKE, so revoking through it must be
    // refused: a borrowed handle may stop using an object but may not disarm
    // anyone else.
    rc = cap_revoke(table, cap_a);
    check(rc == AF_ERR_PERM, "revoke without AF_RIGHT_REVOKE is refused");

    rc = cap_revoke(table, chain);
    check(rc == AF_OK, "revoke with the right succeeds");

    rc = cap_lookup(table, chain, AF_RIGHT_READ, &got);
    check(rc == AF_ERR_CAP_INVALID, "the revoked capability is gone");

    rc = cap_lookup(table, grandchild, AF_RIGHT_READ, &got);
    check(rc == AF_ERR_CAP_INVALID,
          "the GRANDCHILD is gone too — revocation is transitive");

    check(cap_used_slots(table) == before_revoke, "the chain walk released its slots");

    // The parent is untouched: revoking a derived capability must not reach
    // upwards. A process dropping a handle it handed out does not lose its own.
    rc = cap_lookup(table, root, AF_RIGHT_READ, &got);
    check(rc == AF_OK, "revoking a child does NOT revoke its parent");

    // ...and the parent still works on the object, which is the check that it is
    // the same object and not merely a live handle.
    check(got.id == 0x1E00000, "the parent still names the original object");

    // =========================================================================
    // 5. The stale handle — the case a plausible implementation gets wrong
    //
    // Free a slot, reallocate it for a DIFFERENT object, then present the old
    // handle. Without the generation check this succeeds and the holder has
    // silently acquired a capability to something it was never given.
    // =========================================================================
    af_cap_t doomed = cap_insert(table, frame_obj(0x2A00000), AF_RIGHT_READ);
    check(doomed != AF_CAP_INVALID, "a capability was issued for the stale test");

    check(cap_delete(table, doomed) == AF_OK, "it can be deleted");

    // Reuse the path: the next insert takes the same slot, because insertion
    // searches from the bottom.
    af_cap_t replacement = cap_insert(table, frame_obj(0x2B00000), AF_RIGHT_WRITE);
    check(replacement != AF_CAP_INVALID, "a replacement capability was issued");
    check((replacement & AF_CAP_INDEX_MASK) == (doomed & AF_CAP_INDEX_MASK),
          "the replacement reuses the same SLOT");
    check(replacement != doomed, "but its handle is DIFFERENT — the generation moved");

    rc = cap_lookup(table, doomed, AF_RIGHT_READ, &got);
    check(rc == AF_ERR_CAP_INVALID,
          "the STALE handle fails rather than reaching the new object");

    rc = cap_lookup(table, replacement, AF_RIGHT_WRITE, &got);
    check(rc == AF_OK, "the new handle works");
    check(got.id == 0x2B00000, "and names the new object, not the old one");

    // =========================================================================
    // 6. Exhaustion and the table boundary
    // =========================================================================
    af_cap_table_t *small = cap_table_create();
    check(small != NULL, "a second table can be created");

    af_u32 inserted = 0;
    for (af_u32 i = 0; i < AF_CAP_MAX_SLOTS + 10; i++) {
        if (cap_insert(small, frame_obj(0x1000 + i), AF_RIGHT_READ) ==
            AF_CAP_INVALID) {
            break;
        }
        inserted++;
    }

    // Slot 0 is reserved, so the capacity is one less than the slot count.
    check(inserted == AF_CAP_MAX_SLOTS - 1,
          "a table holds exactly AF_CAP_MAX_SLOTS - 1 capabilities");
    check(cap_insert(small, frame_obj(1), AF_RIGHT_READ) == AF_CAP_INVALID,
          "a full table refuses rather than wrapping");
    check(cap_used_slots(small) == AF_CAP_MAX_SLOTS - 1,
          "a refused insert does not corrupt the count");

    cap_table_destroy(small);
    cap_table_destroy(table);

    // Every object type must be nameable, including the one a switch forgets.
    for (af_u32 t = 0; t <= (af_u32)AF_OBJECT_PROCESS; t++) {
        const char *name = cap_object_type_name((af_object_type_t)t);
        s_checks++;
        if (name == NULL || name[0] == '\0' ||
            (name[0] == '?' && name[1] == '\0')) {
            s_failures++;
            af_error("cap", "object type %u has no name", t);
        }
    }

    // =========================================================================
    // Report
    // =========================================================================
    if (s_failures != 0) {
        af_error("cap", "%u of %u capability checks FAILED", s_failures, s_checks);
        af_marker("AF_TEST_FAIL");
        return;
    }

    af_info("cap", "%u capability checks passed", s_checks);
    af_marker("AF_CAP_OK");
}

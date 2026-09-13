// SPDX-License-Identifier: MIT
// AfriyieOS — inter-process communication
//
// Endpoints, and the four primitives built on them. See
// docs/architecture/ipc-protocol.md for the design this implements.
//
// WHY THIS IS THE PIVOT OF THE WHOLE PROJECT
//
// The microkernel has no user-visible ABI beyond capabilities and IPC. Every
// foreign personality (ADR-012) is a user-space server reached through an
// endpoint: the exec server asks a package server to resolve a path, a Linux
// binary's `open()` becomes an IPC call to the file server, a Windows
// `CreateFile` becomes the same call through a different translator.
//
// That is the entire reason "run software from every platform" is a defensible
// goal here. WSL1 welded a Linux translator into NT and every Linux bug became
// a kernel bug. Here a personality is a process that answers messages, and the
// worst a bug in it can do is answer wrongly.
//
// THE MESSAGE IS THE WIRE FORMAT. 64 bytes, no header, no length field, no
// serialisation — it fits exactly in the registers both architectures make
// available for an argument-passing fast path, and the kernel copies it as four
// 128-bit moves where it can.
//
// THE RULE THAT KEEPS IT FAST: if a payload is bigger than a struct, do not copy
// it — grant it. Up to 32 bytes goes in words[]; anything larger is a memory
// grant, which is a capability in caps[] and one mapping. Copying a kilobyte
// through the kernel on every request is how a translation layer becomes slow
// enough to need a second design (ADR-014).

#ifndef AFRIYIE_IPC_H
#define AFRIYIE_IPC_H

#include "types.h"
#include "status.h"
#include "cap.h"

struct af_thread;

// -----------------------------------------------------------------------------
// The message
// -----------------------------------------------------------------------------
#define AF_MSG_MAX_CAPS     4
#define AF_MSG_INLINE_WORDS 4

typedef struct {
    af_u64 label;                          // protocol / method selector
    af_u32 cap_count;                      // how many of caps[] are valid
    af_u32 _pad;
    af_u32 caps[AF_MSG_MAX_CAPS];          // capability handles transferred
    af_u64 words[AF_MSG_INLINE_WORDS];     // 32 bytes of inline payload
} af_msg_t;

// The size is the point, so it is asserted rather than assumed.
//
// AND THE ASSERTION IMMEDIATELY EARNED ITS PLACE. The protocol document declares
// this structure as 64 bytes and writes `af_u64 caps[AF_MSG_MAX_CAPS]` — which
// sums to 8 + 4 + 4 + 32 + 32 = 80, not 64. The document contradicts itself, and
// the contradiction is invisible until somebody adds up the fields.
//
// The fix is not to move the target. 64 bytes is the number of argument bytes
// ARM64 makes available in registers (x0-x7), and it is the threshold the whole
// fast path is designed around; 80 would be a 25% overhead on every message for
// no gain. The fix is that a capability handle is af_u32 — an index with a
// generation packed into it, per cap.h — so storing one as u64 wasted four bytes
// each AND was inconsistent with the capability model.
//
// 8 + 4 + 4 + (4 x 4) + (4 x 8) = 64.
#define AF_MSG_SIZE 64

typedef af_u32 af_ep_t;
#define AF_EP_INVALID 0u

// How many messages an endpoint holds before a sender must wait.
//
// Small on purpose. An unbounded queue turns a slow server into unbounded kernel
// memory growth, which is a denial-of-service a client can trigger by sending;
// eight is enough to absorb a burst and small enough that hitting the limit is
// backpressure rather than a leak.
#define AF_EP_QUEUE_LEN 8

#define AF_EP_NAME_LEN 16

// The word index ipc_call uses to tell a server where to send its reply.
//
// TEMPORARY, AND NAMED AS SUCH: the reply endpoint travels as a bare id in
// the message rather than as a transferred capability, because moving a
// capability from one process's table to another's is not implemented yet.
// Within one address space the distinction does not arise; across
// processes it does, and this is the thing that has to change.
#define AF_MSG_WORD_REPLY_EP 0u

// -----------------------------------------------------------------------------
// Endpoint lifecycle
// -----------------------------------------------------------------------------

// Creates an endpoint. Returns NULL when the table is full or memory is short.
//
// An endpoint is created by a server and handed to clients as a capability. The
// endpoint itself has no name a client can use — the capability IS the address.
// `name` exists for the boot log, and a client cannot reach an endpoint by
// knowing it.
typedef struct af_endpoint af_endpoint_t;

af_endpoint_t *ipc_endpoint_create(const char *name);

// Takes a reference. Called when a capability to the endpoint is inserted, so
// that the endpoint outlives every individual holder.
void ipc_endpoint_ref(af_endpoint_t *ep);

// Drops a reference and destroys the endpoint at zero.
//
// Destruction matters for a real reason, not tidiness: a server that exits while
// a client is blocked sending to it must not leave the client blocked forever.
// When the last reference goes, every waiter is woken with AF_ERR_NOOBJ. See the
// failure semantics in the protocol document.
void ipc_endpoint_unref(af_endpoint_t *ep);

af_ep_t ipc_endpoint_id(const af_endpoint_t *ep);

// Resolves an endpoint id to the endpoint, or NULL.
//
// KERNEL-INTERNAL, and it exists only because the reply path above carries
// an id instead of a capability. A user program must never be able to name
// an endpoint it does not hold — the capability is the address — so this is
// not exposed through a system call and will be deleted when capability
// transfer lands.
af_endpoint_t *ipc_endpoint_lookup(af_ep_t id);
const char *ipc_endpoint_name(const af_endpoint_t *ep);

// -----------------------------------------------------------------------------
// The four primitives
//
// These take the endpoint directly, because they are the OBJECT operations. The
// authority check happens one layer up, in the *_cap forms below, which is where
// a syscall lands. Keeping the two apart means the object code has no dependency
// on capability semantics, and there is exactly one place that decides whether a
// caller may do this at all.
// -----------------------------------------------------------------------------

// Fire-and-forget. Queues the message and wakes a waiting receiver.
//
// Blocks ONLY when the queue is full — that is backpressure, and the alternative
// (dropping the message) makes a slow server lose requests silently.
af_status_t ipc_send(af_endpoint_t *ep, const af_msg_t *msg);

// Takes the next message. Blocks when the queue is empty.
//
// Returns AF_ERR_NOOBJ if the endpoint was destroyed while waiting, which is how
// a client learns its server died rather than waiting forever.
af_status_t ipc_recv(af_endpoint_t *ep, af_msg_t *out);

// Send and block for the reply, atomically.
//
// ATOMICALLY is the load-bearing word. A reply that arrived between the send and
// the block would be lost by a two-step implementation, and the window is small
// enough that it would pass every test and fail in production under load. The
// reply endpoint is created before the send, so it cannot be missed.
af_status_t ipc_call(af_endpoint_t *ep, const af_msg_t *msg, af_msg_t *reply_out);

// Answers a caller blocked in ipc_call.
af_status_t ipc_reply(af_endpoint_t *reply_ep, const af_msg_t *msg);

// -----------------------------------------------------------------------------
// Capability-checked forms
//
// What a system call lands on. The rights are the point:
//   AF_RIGHT_SIGNAL  may send or call
//   AF_RIGHT_WAIT    may receive
//
// Both errors stay distinguishable — AF_ERR_CAP_INVALID for a stale handle,
// AF_ERR_PERM for a live handle without the right — for the same reason as in
// cap.c: a permissions bug and a use-after-free have nothing in common except a
// failing test, and collapsing them makes one of them invisible.
// -----------------------------------------------------------------------------
af_status_t ipc_send_cap(af_cap_table_t *table, af_cap_t cap,
                         const af_msg_t *msg);
af_status_t ipc_recv_cap(af_cap_table_t *table, af_cap_t cap,
                         af_msg_t *out);
af_status_t ipc_call_cap(af_cap_table_t *table, af_cap_t cap,
                         const af_msg_t *msg, af_msg_t *reply_out);

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
void ipc_dump(void);
af_u32 ipc_live_endpoints(void);
af_u32 ipc_messages_sent(void);

// Boot-time checks. Runs AFTER the scheduler is up, because half of what it
// tests is blocking and a test of blocking without threads tests nothing.
// Emits AF_IPC_OK.
void af_ipc_selftest(void);

#endif // AFRIYIE_IPC_H

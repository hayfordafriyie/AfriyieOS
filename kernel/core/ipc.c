// SPDX-License-Identifier: MIT
// AfriyieOS — IPC implementation
//
// Endpoint objects and the four primitives. The design is in ipc.h and
// docs/architecture/ipc-protocol.md; this file is the mechanics.
//
// THE ONE INVARIANT THAT MATTERS: a blocked thread is always woken, and always
// for a reason it can act on. There are exactly three ways a wait ends —
//
//     a message arrives       -> AF_OK
//     the endpoint is destroyed -> AF_ERR_NOOBJ  (the server died)
//     a transient wakeup      -> loop and wait again
//
// — and the third is why the waits are written as loops rather than as single
// blocks. A wakeup is not a promise: it means "look again". Code that treats it
// as a promise works perfectly until two threads race, which is to say it works
// until it matters.

#include "afriyie/ipc.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/heap.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

#define AF_MAX_ENDPOINTS 64

AF_STATIC_ASSERT(sizeof(af_msg_t) == AF_MSG_SIZE,
                 "af_msg_t must stay exactly 64 bytes — the message IS the "
                 "register-passing wire format, and growing it silently moves "
                 "every IPC onto the slow path");

// -----------------------------------------------------------------------------
// The endpoint
// -----------------------------------------------------------------------------
struct af_endpoint {
    bool          used;
    af_ep_t       id;
    af_u32        refs;          // holders: capabilities, plus the creator

    char          name[AF_EP_NAME_LEN];

    // A ring buffer. head is the next message to take, tail the next slot to
    // fill, count what is actually queued — deriving any of the three from the
    // other two is how a full queue and an empty queue end up indistinguishable.
    af_msg_t      queue[AF_EP_QUEUE_LEN];
    af_u32        head;
    af_u32        tail;
    af_u32        count;

    // Threads blocked in ipc_recv. A LIST, not a single pointer.
    //
    // A single waiter is simpler and was the first design, but a service with
    // two threads receiving would have the second one's wait silently replace
    // the first's — and a lost waiter is a thread that never wakes, which is a
    // deadlock that looks like a hang in an unrelated subsystem. The list is
    // chained through af_thread.wait_next.
    struct af_thread *waiters;

    // Threads blocked in ipc_send because the queue was full.
    struct af_thread *senders;
};

static struct af_endpoint s_endpoints[AF_MAX_ENDPOINTS];
static af_u32             s_live;
static af_ep_t            s_next_id = 1;
static af_u32             s_sent;

// -----------------------------------------------------------------------------
// Wait-list plumbing
//
// Chained through af_thread.wait_next. Append rather than push: a FIFO order
// means the receiver that has waited longest is served first, which is what
// makes the ordering fair rather than merely defined.
// -----------------------------------------------------------------------------
static void waiter_add(struct af_thread **list, struct af_thread *thread)
{
    thread->wait_next = NULL;

    if (*list == NULL) {
        *list = thread;
        return;
    }

    struct af_thread *tail = *list;
    while (tail->wait_next != NULL) {
        tail = tail->wait_next;
    }
    tail->wait_next = thread;
}

static void waiter_remove(struct af_thread **list, struct af_thread *thread)
{
    struct af_thread **link = list;
    while (*link != NULL) {
        if (*link == thread) {
            *link = thread->wait_next;
            thread->wait_next = NULL;
            return;
        }
        link = &(*link)->wait_next;
    }
}

static struct af_thread *waiter_pop(struct af_thread **list)
{
    struct af_thread *thread = *list;
    if (thread != NULL) {
        *list = thread->wait_next;
        thread->wait_next = NULL;
    }
    return thread;
}

// -----------------------------------------------------------------------------
// Lookup
// -----------------------------------------------------------------------------
static af_endpoint_t *endpoint_from_id(af_ep_t id)
{
    if (id == AF_EP_INVALID) {
        return NULL;
    }

    for (af_u32 i = 0; i < AF_MAX_ENDPOINTS; i++) {
        if (s_endpoints[i].used && s_endpoints[i].id == id) {
            return &s_endpoints[i];
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
af_endpoint_t *ipc_endpoint_create(const char *name)
{
    struct af_endpoint *ep = NULL;

    for (af_u32 i = 0; i < AF_MAX_ENDPOINTS; i++) {
        if (!s_endpoints[i].used) {
            ep = &s_endpoints[i];
            break;
        }
    }

    if (ep == NULL) {
        af_error("ipc", "endpoint table is full (%u endpoints)",
                 (af_u32)AF_MAX_ENDPOINTS);
        return NULL;
    }

    af_memset(ep, 0, sizeof(*ep));

    ep->used = true;
    ep->id   = s_next_id++;
    ep->refs = 1;               // the creator's reference

    af_strlcpy(ep->name, (name != NULL) ? name : "?", sizeof(ep->name));

    s_live++;

    af_log(AF_LOG_DEBUG, "ipc", "endpoint %u '%s' created", ep->id, ep->name);
    return ep;
}

void ipc_endpoint_ref(af_endpoint_t *ep)
{
    if (ep != NULL && ep->used) {
        ep->refs++;
    }
}

void ipc_endpoint_unref(af_endpoint_t *ep)
{
    if (ep == NULL || !ep->used || ep->refs == 0) {
        return;
    }

    ep->refs--;
    if (ep->refs > 0) {
        return;
    }

    // --- the last reference is gone ------------------------------------------
    //
    // Waking every waiter is the whole point of reference counting here. A
    // client blocked in ipc_recv on a server that exited must be told, or it
    // waits forever and the process appears hung with no diagnostic anywhere.
    struct af_thread *t;

    while ((t = waiter_pop(&ep->waiters)) != NULL) {
        af_warn("ipc", "endpoint %u destroyed under a blocked receiver "
                       "(tid %u) — waking it with AF_ERR_NOOBJ",
                ep->id, t->tid);
        sched_unblock(t);
    }

    while ((t = waiter_pop(&ep->senders)) != NULL) {
        af_warn("ipc", "endpoint %u destroyed under a blocked sender "
                       "(tid %u) — waking it with AF_ERR_NOOBJ",
                ep->id, t->tid);
        sched_unblock(t);
    }

    af_log(AF_LOG_DEBUG, "ipc", "endpoint %u '%s' destroyed", ep->id, ep->name);

    ep->used = false;
    ep->id   = AF_EP_INVALID;

    if (s_live > 0) {
        s_live--;
    }
}

af_endpoint_t *ipc_endpoint_lookup(af_ep_t id)
{
    return endpoint_from_id(id);
}

af_ep_t ipc_endpoint_id(const af_endpoint_t *ep)
{
    return (ep != NULL) ? ep->id : AF_EP_INVALID;
}

const char *ipc_endpoint_name(const af_endpoint_t *ep)
{
    return (ep != NULL) ? ep->name : "?";
}

// -----------------------------------------------------------------------------
// Send
//
// Called with the endpoint's state consistent and no lock held across a block.
// The sequence is deliberate:
//
//   1. if there is a waiting receiver, hand the message straight over
//   2. otherwise, if there is room, queue it
//   3. otherwise, block until a slot frees
//
// Step 1 before step 2 matters for latency, not correctness: a message that goes
// straight to a waiting server never touches the queue at all, which is the
// common case for a request/response service.
// -----------------------------------------------------------------------------
af_status_t ipc_send(af_endpoint_t *ep, const af_msg_t *msg)
{
    if (ep == NULL || msg == NULL) {
        return AF_ERR_INVAL;
    }

    while (true) {
        if (!ep->used) {
            return AF_ERR_NOOBJ;
        }

        // --- a receiver is waiting: hand it over directly --------------------
        struct af_thread *receiver = waiter_pop(&ep->waiters);
        if (receiver != NULL) {
            // The message goes into the endpoint's queue slot anyway, and the
            // receiver picks it up on its way out of the block. Copying it into
            // the receiver's structure would mean a second path that delivers
            // messages, and two delivery paths is two places for the queue
            // accounting to disagree.
            ep->queue[ep->tail] = *msg;
            ep->tail = (ep->tail + 1u) % AF_EP_QUEUE_LEN;
            ep->count++;

            s_sent++;
            sched_unblock(receiver);
            return AF_OK;
        }

        // --- room in the queue ------------------------------------------------
        if (ep->count < AF_EP_QUEUE_LEN) {
            ep->queue[ep->tail] = *msg;
            ep->tail = (ep->tail + 1u) % AF_EP_QUEUE_LEN;
            ep->count++;

            s_sent++;
            return AF_OK;
        }

        // --- full: backpressure -------------------------------------------------
        //
        // A kernel thread blocking here would wedge the reaper, so it is refused
        // rather than queued. That is not a special case for tidiness: the idle
        // thread is what frees every other thread's stack.
        af_thread_t *self = thread_current();
        if (self == NULL || self == sched_idle_thread()) {
            return AF_ERR_AGAIN;
        }

        waiter_add(&ep->senders, self);
        sched_block(ep);

        // Woken. Loop: a wakeup says "look again", not "there is room".
        if (ep->senders != NULL) {
            waiter_remove(&ep->senders, self);
        }
    }
}

// -----------------------------------------------------------------------------
// Receive
// -----------------------------------------------------------------------------
af_status_t ipc_recv(af_endpoint_t *ep, af_msg_t *out)
{
    if (ep == NULL || out == NULL) {
        return AF_ERR_INVAL;
    }

    while (true) {
        if (!ep->used) {
            return AF_ERR_NOOBJ;
        }

        if (ep->count > 0) {
            *out = ep->queue[ep->head];
            ep->head = (ep->head + 1u) % AF_EP_QUEUE_LEN;
            ep->count--;

            // A sender was waiting for this slot. Wake one — and only one, or
            // N senders wake for one free slot and all but one find it full
            // again, which is correct but does N times the work.
            struct af_thread *sender = waiter_pop(&ep->senders);
            if (sender != NULL) {
                sched_unblock(sender);
            }

            return AF_OK;
        }

        af_thread_t *self = thread_current();
        if (self == NULL || self == sched_idle_thread()) {
            return AF_ERR_AGAIN;
        }

        waiter_add(&ep->waiters, self);
        sched_block(ep);

        if (ep->waiters != NULL) {
            waiter_remove(&ep->waiters, self);
        }
    }
}

// -----------------------------------------------------------------------------
// Call and reply
//
// A call is a send plus a blocking receive on a PRIVATE endpoint created before
// the send. The order is the whole design of it:
//
//     create reply endpoint
//     send the request, carrying the reply endpoint's id
//     block on the reply endpoint
//
// A two-step version — send, then create and announce a reply address — has a
// window in which the server can answer before the caller is listening. The
// window is microseconds wide and would pass every test written on a quiet
// machine, then lose replies under load.
//
// The reply endpoint's id is carried in words[AF_MSG_WORD_REPLY_EP], not as a
// capability — see the note on that constant in ipc.h for why, and for what
// has to change.
// -----------------------------------------------------------------------------

af_status_t ipc_call(af_endpoint_t *ep, const af_msg_t *msg, af_msg_t *reply_out)
{
    if (ep == NULL || msg == NULL || reply_out == NULL) {
        return AF_ERR_INVAL;
    }

    af_endpoint_t *reply_ep = ipc_endpoint_create("reply");
    if (reply_ep == NULL) {
        return AF_ERR_NOMEM;
    }

    af_msg_t request = *msg;
    request.words[AF_MSG_WORD_REPLY_EP] = ipc_endpoint_id(reply_ep);

    const af_status_t rc = ipc_send(ep, &request);
    if (af_status_err(rc)) {
        ipc_endpoint_unref(reply_ep);
        return rc;
    }

    const af_status_t got = ipc_recv(reply_ep, reply_out);

    ipc_endpoint_unref(reply_ep);
    return got;
}

af_status_t ipc_reply(af_endpoint_t *reply_ep, const af_msg_t *msg)
{
    // A reply never blocks on a full queue in the intended design: the caller is
    // blocked waiting for exactly this message and the reply endpoint has no
    // other user, so the queue cannot be full unless the caller died between
    // sending and replying — in which case ipc_send returns AF_ERR_NOOBJ and the
    // server learns the truth, which is better than blocking on a dead client.
    return ipc_send(reply_ep, msg);
}

// -----------------------------------------------------------------------------
// Capability-checked forms
// -----------------------------------------------------------------------------
static af_status_t endpoint_from_cap(af_cap_table_t *table, af_cap_t cap,
                                     af_u32 required, af_endpoint_t **out)
{
    af_object_t object;
    const af_status_t rc = cap_lookup(table, cap, required, &object);
    if (af_status_err(rc)) {
        return rc;
    }

    // A capability naming something that is not an endpoint is a type error, not
    // a permission error, and reporting AF_ERR_INVAL rather than proceeding to
    // treat a frame id as an endpoint id is the difference between a caught bug
    // and a wild pointer.
    if (object.type != AF_OBJECT_ENDPOINT) {
        return AF_ERR_INVAL;
    }

    af_endpoint_t *ep = endpoint_from_id((af_ep_t)object.id);
    if (ep == NULL) {
        return AF_ERR_NOOBJ;
    }

    *out = ep;
    return AF_OK;
}

af_status_t ipc_send_cap(af_cap_table_t *table, af_cap_t cap,
                         const af_msg_t *msg)
{
    af_endpoint_t *ep = NULL;
    af_status_t rc = endpoint_from_cap(table, cap, AF_RIGHT_SIGNAL, &ep);
    if (af_status_err(rc)) {
        return rc;
    }
    return ipc_send(ep, msg);
}

af_status_t ipc_recv_cap(af_cap_table_t *table, af_cap_t cap, af_msg_t *out)
{
    af_endpoint_t *ep = NULL;
    af_status_t rc = endpoint_from_cap(table, cap, AF_RIGHT_WAIT, &ep);
    if (af_status_err(rc)) {
        return rc;
    }
    return ipc_recv(ep, out);
}

af_status_t ipc_call_cap(af_cap_table_t *table, af_cap_t cap,
                         const af_msg_t *msg, af_msg_t *reply_out)
{
    af_endpoint_t *ep = NULL;
    af_status_t rc = endpoint_from_cap(table, cap, AF_RIGHT_SIGNAL, &ep);
    if (af_status_err(rc)) {
        return rc;
    }
    return ipc_call(ep, msg, reply_out);
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
af_u32 ipc_live_endpoints(void) { return s_live; }
af_u32 ipc_messages_sent(void)  { return s_sent; }

void ipc_dump(void)
{
    af_info("ipc", "%u endpoint(s) live, %u message(s) sent", s_live, s_sent);

    for (af_u32 i = 0; i < AF_MAX_ENDPOINTS; i++) {
        const struct af_endpoint *ep = &s_endpoints[i];
        if (!ep->used) {
            continue;
        }
        af_info("ipc", "  ep %u '%s' refs %u queued %u waiters",
                ep->id, ep->name, ep->refs, ep->count);
    }
}

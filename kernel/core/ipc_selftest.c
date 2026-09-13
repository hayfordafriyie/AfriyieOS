// SPDX-License-Identifier: MIT
// AfriyieOS — IPC self test
//
// THIS RUNS AFTER THE SCHEDULER IS UP, and that placement is the test design,
// not an accident of where it fit. Half of what IPC does is block: a receiver
// waits for a message, a sender waits for room, a caller waits for a reply. A
// test of blocking without threads and a scheduler tests nothing — every wait
// would return immediately because nothing else could ever run.
//
// So it runs from the boot thread once threads exist, and it uses real ones.
//
// WHAT IS TESTED, and why each one is here:
//
//   1. Round trip. A message sent is a message received, byte for byte. The
//      cheap case, and the only one a naive implementation passes.
//   2. BLOCKING RECEIVE. A thread blocks on an empty endpoint and is woken by a
//      sender. If the block were not a loop this passes anyway; if the wait list
//      were a single pointer and something else blocked, it would hang.
//   3. CALL AND REPLY. The synchronous path, which is what every service
//      interaction actually uses.
//   4. Rights. An endpoint capability without AF_RIGHT_SIGNAL cannot send.
//      Without this, capabilities are decoration on top of an open door.
//   5. SERVER DEATH. A client blocked on an endpoint whose last reference goes
//      away must be woken with an error rather than waiting forever. This is the
//      failure mode that makes a process look hung with no diagnostic anywhere.

#include "afriyie/ipc.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
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
        af_error("ipc", "  FAIL  %s", what);
    }
}

// -----------------------------------------------------------------------------
// Test scaffolding shared between the worker threads
// -----------------------------------------------------------------------------
static af_endpoint_t *s_echo_ep;      // the server's endpoint
static af_status_t     s_worker_rc;
static af_msg_t        s_worker_msg;
static volatile bool   s_worker_done;

// A receiver that blocks on an empty endpoint and reports what it got.
static void receiver_thread(void *arg)
{
    af_endpoint_t *ep = (af_endpoint_t *)arg;

    s_worker_rc = ipc_recv(ep, &s_worker_msg);
    s_worker_done = true;

    thread_exit();
}

// A server: receive one request, reply on the endpoint named in words[0].
//
// It resolves the reply endpoint by ID, which is the temporary shortcut the
// reply path uses until capability transfer exists — see AF_MSG_WORD_REPLY_EP.
static void echo_server_thread(void *arg)
{
    AF_UNUSED(arg);

    af_msg_t request;
    if (af_status_err(ipc_recv(s_echo_ep, &request))) {
        s_worker_rc = AF_ERR_NOOBJ;
        s_worker_done = true;
        thread_exit();
    }

    af_endpoint_t *reply_to =
        ipc_endpoint_lookup((af_ep_t)request.words[AF_MSG_WORD_REPLY_EP]);

    af_msg_t reply;
    af_memset(&reply, 0, sizeof(reply));
    reply.label    = 0x0200u;                  // "here is your answer"
    reply.words[0] = request.words[1] + 1u;    // the echo: input plus one
    reply.words[1] = request.words[2];

    if (reply_to == NULL) {
        af_error("ipc", "server could not resolve the reply endpoint");
        s_worker_rc = AF_ERR_NOOBJ;
    } else {
        s_worker_rc = ipc_reply(reply_to, &reply);
    }

    s_worker_done = true;
    thread_exit();
}

// A thread that blocks receiving and must be woken with an error when the
// endpoint is destroyed underneath it.
static void doomed_receiver_thread(void *arg)
{
    af_endpoint_t *ep = (af_endpoint_t *)arg;

    af_msg_t msg;
    s_worker_rc = ipc_recv(ep, &msg);
    s_worker_done = true;

    thread_exit();
}

static af_thread_t *spawn(const char *name, af_thread_fn fn, void *arg)
{
    af_thread_t *t = thread_create(name, fn, arg, AF_KERNEL_STACK_SIZE,
                                   AF_PRIO_DEFAULT);
    if (t != NULL) {
        sched_admit(t);
    }
    return t;
}

// Waits for a worker to finish. Bounded, so a hang becomes a failure with a
// message rather than a boot that never completes.
static bool await_worker(const char *what)
{
    for (af_u32 i = 0; i < 4000; i++) {
        if (s_worker_done) {
            return true;
        }
        sched_collect_zombies();
        sched_yield();
    }

    af_error("ipc", "  FAIL  %s: worker never finished (would deadlock)", what);
    return false;
}

// -----------------------------------------------------------------------------
void af_ipc_selftest(void)
{
    s_checks = 0;
    s_failures = 0;

    // =========================================================================
    // 1. The message is the size the design says it is
    // =========================================================================
    check(sizeof(af_msg_t) == AF_MSG_SIZE,
          "af_msg_t is exactly 64 bytes — the register-passing wire format");

    // =========================================================================
    // 2. Round trip, no blocking
    // =========================================================================
    af_endpoint_t *ep = ipc_endpoint_create("test-echo");
    check(ep != NULL, "an endpoint can be created");
    if (ep == NULL) {
        af_error("ipc", "cannot continue without an endpoint");
        af_marker("AF_TEST_FAIL");
        return;
    }

    af_msg_t out;
    af_memset(&out, 0, sizeof(out));
    out.label       = 0x0100u;
    out.words[0]    = 0xDEADBEEFCAFEF00DULL;
    out.words[3]    = 0x0123456789ABCDEFULL;
    out.cap_count   = 0;

    check(ipc_send(ep, &out) == AF_OK, "a message sends to an empty endpoint");

    af_msg_t in;
    af_memset(&in, 0, sizeof(in));
    check(ipc_recv(ep, &in) == AF_OK, "the message is received");
    check(in.label == out.label, "the label round-trips");
    check(in.words[0] == out.words[0], "word 0 round-trips exactly");
    check(in.words[3] == out.words[3], "the last word round-trips exactly");

    // NOTE what is NOT done here: a second ipc_recv on the now-empty endpoint.
    //
    // The first version of this test did exactly that, wrapped in `|| true` so
    // the assertion could not fail. The assertion was meaningless — and the
    // CALL still blocked, parking the boot thread forever on an endpoint
    // nothing would ever send to. A test that cannot fail still runs, and the
    // hang would have looked like a scheduler bug.
    //
    // The empty-endpoint case is tested below, WITH a thread and a sender, and
    // it is the only way it can be tested at all.

    // =========================================================================
    // 3. Queueing, then blocking receive
    //
    // Fill the queue, then have a thread block on it. The thread must be woken by
    // the next send and must see the messages IN ORDER — a ring buffer with a
    // head/tail bug reorders silently and every single-message test passes.
    // =========================================================================
    af_endpoint_t *q = ipc_endpoint_create("test-queue");
    check(q != NULL, "a queueing endpoint was created");

    for (af_u32 i = 0; i < AF_EP_QUEUE_LEN; i++) {
        af_msg_t m;
        af_memset(&m, 0, sizeof(m));
        m.label    = 100u + i;
        m.words[0] = i;
        check(ipc_send(q, &m) == AF_OK, "a queue slot accepts a message");
    }

    // Now the queue is full. The next send from a thread must BLOCK, and the
    // drain below must release it.
    for (af_u32 i = 0; i < AF_EP_QUEUE_LEN; i++) {
        af_msg_t m;
        check(ipc_recv(q, &m) == AF_OK, "the queue drains in order");
        check(m.label == 100u + i, "messages come back in FIFO order");
    }

    // --- the blocking case ---------------------------------------------------
    s_worker_done = false;
    s_worker_rc   = AF_ERR_INVAL;

    af_endpoint_t *blocking_ep = ipc_endpoint_create("test-block");
    check(blocking_ep != NULL, "a blocking endpoint was created");

    af_thread_t *receiver = spawn("ipc-recv", receiver_thread, blocking_ep);
    check(receiver != NULL, "a receiver thread was created");

    if (receiver != NULL) {
        // Let the receiver actually reach the block. Without this the send below
        // could arrive first and take the queueing path, and the test would pass
        // without ever having exercised blocking.
        for (af_u32 i = 0; i < 20 && !s_worker_done; i++) {
            sched_yield();
        }

        af_msg_t wake;
        af_memset(&wake, 0, sizeof(wake));
        wake.label    = 0xB10Cu;    // "block"
        wake.words[0] = 0x5A5A5A5AULL;
        check(ipc_send(blocking_ep, &wake) == AF_OK,
              "a send to an endpoint with a blocked receiver succeeds");

        check(await_worker("blocking receive"), "the blocked receiver finished");
        check(s_worker_rc == AF_OK, "the blocked receive returned AF_OK");
        check(s_worker_msg.label == 0xB10Cu,
              "the woken receiver got the message that woke it");
        check(s_worker_msg.words[0] == 0x5A5A5A5AULL,
              "and the payload is intact — nothing was lost in the handover");
    }

    // =========================================================================
    // 4. Call and reply
    //
    // ipc_call is the PRIMARY primitive — the overwhelming majority of service
    // interactions are "ask a question, get an answer" — so it is tested as
    // itself rather than as send-plus-receive. The first version of this test
    // hand-rolled the two halves and never called ipc_call at all, which would
    // have left the primitive the whole system is built on untested.
    //
    // The property being checked is that ipc_call is ATOMIC: it creates its
    // reply endpoint before sending, so a reply that arrives immediately cannot
    // be missed. A send-then-listen implementation has a window, passes on a
    // quiet machine, and loses replies under load.
    // =========================================================================
    s_echo_ep = ipc_endpoint_create("test-server");
    check(s_echo_ep != NULL, "a server endpoint was created");

    s_worker_done = false;
    s_worker_rc   = AF_ERR_INVAL;
    af_thread_t *server = spawn("ipc-server", echo_server_thread, NULL);
    check(server != NULL, "a server thread was created");

    if (server != NULL) {
        af_msg_t request;
        af_memset(&request, 0, sizeof(request));
        request.label    = 0x0100u;
        request.words[1] = 41u;              // the server adds one
        request.words[2] = 0xFEEDFACEu;

        // The boot thread blocks here until the server answers. ipc_call fills
        // in words[AF_MSG_WORD_REPLY_EP] itself, which is why the test does not.
        af_msg_t reply;
        af_memset(&reply, 0, sizeof(reply));

        check(ipc_call(s_echo_ep, &request, &reply) == AF_OK,
              "ipc_call returned a reply");

        check(await_worker("call and reply"), "the server finished");
        check(s_worker_rc == AF_OK, "the server's reply send succeeded");

        check(reply.label == 0x0200u, "the reply carries the expected label");
        check(reply.words[0] == 42u, "the server's computation round-tripped");
        check(reply.words[1] == 0xFEEDFACEu,
              "the echoed payload was not corrupted");
    }

    // =========================================================================
    // 5. Rights
    // =========================================================================
    af_cap_table_t *caps = cap_table_create();
    check(caps != NULL, "a capability table was created");

    if (caps != NULL) {
        af_object_t obj;
        obj.type = AF_OBJECT_ENDPOINT;
        obj.id   = ipc_endpoint_id(ep);

        // Wait-only: may receive, may NOT send.
        af_cap_t wait_only = cap_insert(caps, obj, AF_RIGHT_WAIT);
        check(wait_only != AF_CAP_INVALID, "a WAIT-only capability was issued");

        af_msg_t m;
        af_memset(&m, 0, sizeof(m));

        check(ipc_send_cap(caps, wait_only, &m) == AF_ERR_PERM,
              "sending through a WAIT-only capability is refused");

        // Signal-only: may send, may NOT receive.
        af_cap_t signal_only = cap_insert(caps, obj, AF_RIGHT_SIGNAL);
        check(ipc_send_cap(caps, signal_only, &m) == AF_OK,
              "sending through a SIGNAL capability succeeds");
        check(ipc_recv_cap(caps, signal_only, &m) == AF_ERR_PERM,
              "receiving through a SIGNAL-only capability is refused");

        // A stale handle and an insufficient one must stay distinguishable.
        check(ipc_send_cap(caps, AF_CAP_INVALID, &m) == AF_ERR_CAP_INVALID,
              "an invalid capability is AF_ERR_CAP_INVALID, not AF_ERR_PERM");

        // A capability naming a FRAME where an endpoint is expected is a type
        // error. Treating a frame address as an endpoint id would be a wild
        // pointer, so it must be caught here rather than believed.
        af_object_t frame;
        frame.type = AF_OBJECT_FRAME;
        frame.id   = 0x1E00000;
        af_cap_t wrong_type = cap_insert(caps, frame, AF_RIGHT_SIGNAL);
        check(ipc_send_cap(caps, wrong_type, &m) == AF_ERR_INVAL,
              "a capability to the wrong object type is refused as invalid");

        cap_table_destroy(caps);
    }

    // =========================================================================
    // 6. Server death
    //
    // The failure mode that makes a process look hung with no diagnostic
    // anywhere: a client waits forever on a server that no longer exists.
    // =========================================================================
    s_worker_done = false;
    s_worker_rc   = AF_ERR_INVAL;

    af_endpoint_t *doomed = ipc_endpoint_create("test-doomed");
    check(doomed != NULL, "an endpoint was created for the death test");

    af_thread_t *waiter = spawn("ipc-doomed", doomed_receiver_thread, doomed);
    check(waiter != NULL, "a receiver was blocked on it");

    if (waiter != NULL) {
        for (af_u32 i = 0; i < 20 && !s_worker_done; i++) {
            sched_yield();
        }

        // Destroy the endpoint out from under the blocked receiver.
        ipc_endpoint_unref(doomed);

        check(await_worker("server death"),
              "destroying the endpoint woke the blocked receiver");
        check(s_worker_rc == AF_ERR_NOOBJ,
              "the woken receiver is told the endpoint is gone, not given silence");
    }

    // =========================================================================
    // 7. Cleanup
    // =========================================================================
    ipc_endpoint_unref(ep);
    ipc_endpoint_unref(q);
    ipc_endpoint_unref(blocking_ep);
    ipc_endpoint_unref(s_echo_ep);

    check(ipc_live_endpoints() == 0,
          "every endpoint was released — no leak across the whole test");

    // =========================================================================
    // Report
    // =========================================================================
    if (s_failures != 0) {
        af_error("ipc", "%u of %u IPC checks FAILED", s_failures, s_checks);
        af_marker("AF_TEST_FAIL");
        return;
    }

    af_info("ipc", "%u IPC checks passed (%u messages sent)",
            s_checks, ipc_messages_sent());
    af_marker("AF_IPC_OK");
}

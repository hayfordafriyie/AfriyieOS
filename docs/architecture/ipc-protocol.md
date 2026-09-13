# IPC Protocol

**Status:** 📐 designed · 📐 implemented at v0.7

Inter-process communication is the heart of a microkernel. Everything that is not
scheduling, address spaces or interrupt routing is *some other process being
asked to do something* — so IPC latency and correctness set the ceiling on how
good the whole system can be.

---

## 1. Design goals, in priority order

1. **The fast path must be fast.** A `call`/`reply` round trip is the single most
   common operation in the system. Target: **under 2 000 cycles.**
2. **No allocation, no copy on the fast path.** A 64-byte message travels in CPU
   registers.
3. **Large transfers are shared, not copied.** Zero-copy via page grants.
4. **Everything is capability-checked.** There is no global namespace.
5. **A dead peer fails cleanly.** A caller blocked on a dead server gets
   `AF_ERR_PEER_DEAD`, never a hang.

---

## 2. Objects

```
    ENDPOINT  ── a message queue plus a block-on-receive queue
        │
        ├── owns:  a ring of pending messages
        │          a list of threads blocked in ipc_recv
        │          a list of threads blocked in ipc_call (awaiting reply)
        │
        └── reached only through a capability in a process's capability table
```

An endpoint is created by the kernel and handed to a process as a capability.
A process can hold `CAP_READ` (may receive), `CAP_WRITE` (may send),
`CAP_GRANT` (may pass it on) or any combination. A thread that holds only
`CAP_WRITE` can send to a server but cannot impersonate it.

A **reply object** is a single-use capability captured when a thread blocks in
`ipc_call`. It exists so that a reply cannot be sent by anyone other than the
peer that received the call, and so a server cannot reply twice or reply to the
wrong caller.

---

## 3. The message

```c
/* kernel/include/afriyie/ipc.h */
#define AF_MSG_MAX_CAPS     4
#define AF_MSG_INLINE_WORDS 4

typedef struct {
    af_u64 label;                          /* protocol / method selector  */
    af_u32 cap_count;                      /* how many of caps[] are valid */
    af_u32 _pad;
    af_u32 caps[AF_MSG_MAX_CAPS];          /* capability handles           */
    af_u64 words[AF_MSG_INLINE_WORDS];     /* 32 bytes of inline payload   */
} af_msg_t;                                /* 64 bytes */
```

64 bytes, the number of argument bytes ARM64 makes available in registers
(`x0`–`x7`), which is what the fast path is designed around. There is no header,
no length field and no serialisation: the structure *is* the wire format.

> **This table previously said `af_u64 caps[...]` and still claimed 64 bytes.**
> It does not add up: 8 + 4 + 4 + 32 + 32 = **80**. The contradiction sat here
> from the first draft and was found by a `_Static_assert` on the structure's
> size when the header was finally implemented — the kind of error that is
> invisible to reading and obvious to arithmetic.
>
> The fix was not to move the target. 64 is the register count the design turns
> on; 80 would be a 25% overhead on every message for nothing. The fix is that a
> capability handle is `af_u32` — an index with a generation packed into it, per
> [capability-model.md](../architecture/capability-model.md) — so storing one as
> `u64` both wasted four bytes each and contradicted the capability model.

### 3.1 Choosing inline words or a grant

| Payload | Mechanism | Cost |
| --- | --- | --- |
| ≤ 32 bytes | `words[]` | registers only |
| 33 B – 1 page | memory grant, capability in `caps[]` | one mapping, no copy |
| > 1 page | memory grant of a shared region | one mapping, no copy |
| A file descriptor / device | capability in `caps[]` with narrowed rights | registers only |

The rule: **if it is bigger than a struct, do not copy it — grant it.**

---

## 4. The four primitives

| Primitive | Semantics | Blocks? |
| --- | --- | --- |
| `ipc_send(cap, msg)` | Fire-and-forget. Queued on the endpoint. | Only if the queue is full |
| `ipc_recv(cap, &msg)` | Take the next message. | Yes, when empty |
| `ipc_call(cap, msg, &reply)` | Send and block for the reply, atomically. | Yes |
| `ipc_reply(reply_cap, msg)` | Answer a caller blocked in `ipc_call`. | No |

### 4.1 Why synchronous `call` is the primary path

A purely asynchronous model needs state machines at every call site and makes
error handling baroque. The overwhelming majority of service interactions are
"ask a question, get an answer", and making that the *primitive* rather than
something built out of two primitives is what keeps the fast path under budget.

Asynchronous `send` still exists for notifications and event streams, so nothing
is lost.

### 4.2 The fast path

```
   client thread                    kernel                    server thread
        │
        │ ipc_call(cap, msg, &reply)
        ├──────────────────────────►│
        │                           │ validate cap
        │                           │ target endpoint has a waiter?
        │                           │   yes ─► copy 64 bytes into the waiter's
        │                           │          message registers
        │                           │          capture a reply capability
        │                           │          switch directly to the server
        │                           │            (bypass the scheduler)
        │                           │   no  ─► enqueue; the server is scheduled
        │                           │          normally
        │                           │
        │                           │◄─────────────── ipc_reply(reply_cap, msg)
        │◄──────────────────────────│ copy 64 bytes back, wake the caller
        │
```

Two deliberate choices:

* **Direct switch on `call`.** When the server is already blocked in `ipc_recv`,
  the kernel hands the CPU straight to it rather than putting the client on the
  run queue and waiting for the scheduler. This is what makes the round trip
  cheap: one context switch, not three.
* **Priority inheritance on the reply path.** If a high-priority client calls a
  low-priority server, the server inherits the client's priority for the duration
  of the call. Without this, priority inversion makes the client wait behind
  everything the server outranks — the classic Mars Pathfinder failure.

---

## 5. Memory grants

The zero-copy path.

```c
/* Allocate a frame-backed region (v0.7) */
af_cap_t frame_cap = sys_mem_alloc(size, AF_MEM_ANON);

/* Map it into the peer's address space with narrowed rights */
af_vaddr peer_addr = sys_mem_grant(peer_process_cap, frame_cap,
                                   0 /* let the kernel choose */,
                                   AF_RIGHT_READ);

/* Send the capability in the message */
af_msg_t msg = { .label = 0x0200_0001 /* FS_READ */, .cap_count = 1 };
msg.caps[0] = frame_cap;
msg.words[0] = peer_addr;
```

Properties that make this safe:

* **Rights may only be narrowed.** A grant of `READ` from a `READ|WRITE` frame
  yields a `READ` mapping. Widening is impossible at every layer.
* **The mapping is per-frame, not per-address.** The receiver gets a copy of the
  mapping, not of the memory.
* **Reference counted.** The frame is freed when the last mapping and the last
  capability are gone.
* **Revocable.** `sys_cap_revoke` on the sender's capability unmaps every derived
  mapping. This is what makes a driver restart safe: revoke everything it was
  given, and nothing it leaked can outlive it.

---

## 6. Notifications

For interrupts and edge-triggered events, a full message queue is the wrong
shape: the receiver wants to know *that* something happened, then ask for detail.

```c
sys_notify(notification_cap, bits);      /* OR a bitmask, non-blocking */
sys_wait(notification_cap, mask, &seen); /* block until any bit in mask is set */
```

An IRQ delivered to a user-space driver becomes a notification, not a message.
The driver wakes, reads the device, and acks. This keeps the interrupt path in
the kernel down to "route the IRQ to a notification" and nothing more.

---

## 7. Protocol namespaces

`label` values are globally allocated so two services cannot collide, and so a
log line naming a label is meaningful without context.

| Range | Service |
| --- | --- |
| `0x0100_xxxx` | Name service |
| `0x0200_xxxx` | File system |
| `0x0300_xxxx` | Device manager |
| `0x0400_xxxx` | Compositor |
| `0x0500_xxxx` | Audio |
| `0x0600_xxxx` | Network |
| `0x0700_xxxx` | Power |
| `0xFF00_xxxx` | Driver SDK |

A protocol is a versioned contract: `0x0200_0001` is `FS_READ` version 1, and
the reply carries the server's supported version so a client can negotiate. This
matters from the moment a service can be upgraded independently of its clients.

### 7.1 Example: reading a file

```
name.lookup("fs")                     → cap ENDPOINT (fs server)
  label 0x0100_0001, words[0..1] = "fs\0"

fs.open(cap, "/home/user/notes.txt")  → cap FRAME + size
  label 0x0200_0002
  words[0] = path length
  words[1] = flags (READ)
  caps[0]  = a granted frame holding the path bytes
  reply: caps[0] = the file handle capability, words[0] = size

fs.read(handle_cap, offset, len)      → cap FRAME granted by the server
  label 0x0200_0003
  reply: caps[0] = frame capability, words[0] = bytes read

The client maps the granted frame, consumes it, and unrefs it.
The file's contents were never copied by either process.
```

---

## 8. Failure semantics

| Situation | Result |
| --- | --- |
| `ipc_call` to a process that exits | `AF_ERR_PEER_DEAD` to the caller, and the reply capability is invalidated |
| `ipc_send` to a full queue | Sender blocks; if the receiver dies while blocked, `AF_ERR_PEER_DEAD` |
| Invalid capability slot | `AF_ERR_CAP_INVALID` |
| Capability without `CAP_WRITE` | `AF_ERR_PERM` |
| Reply to a caller that has gone | Silently discarded; the reply capability is already invalid |
| `ipc_recv` on an endpoint nobody holds | The endpoint is destroyed and the receiver gets `AF_ERR_NOOBJ` |

The invariant: **no IPC operation can block forever because of a peer's death.**
Every blocking path has a wakeup condition that includes "the peer went away".

This is what makes `servers/init` able to supervise: a client blocked on a
crashed server is woken with an error, not left hanging, so it can retry once
init has restarted the server.

---

## 9. Budgets

| Metric | Budget | Measured |
| --- | --- | --- |
| `ipc_call` + `ipc_reply` round trip, warm | < 2 000 cycles | v0.7 |
| `ipc_send` to a non-blocked receiver | < 300 cycles | v0.7 |
| Message copy, 64 bytes inline | 4 × 128-bit moves | v0.7 |
| Memory grant (map + unmap) | < 3 000 cycles | v0.7 |

A microkernel's reputation for slowness is a reputation for *unoptimised* IPC.
The fast path above — register-passed messages, direct switch, priority
inheritance, zero-copy grants — is the standard answer, and these budgets are how
we know whether it worked. They are measured in CI from v0.7 and published in
every release's notes.

---

## 10. Testing

| Test | Tier | What it proves |
| --- | --- | --- |
| 1 000 000 `ipc_call` round trips, mean and p99 logged | T2 | The fast path meets budget |
| Kill a server with clients blocked on it | T3 | Every caller gets `AF_ERR_PEER_DEAD`, no hangs |
| Init restarts the killed server | T3 | The system recovers without a reboot |
| Grant an 8 MiB buffer between two processes | T2 | No copy occurs, contents verified |
| Attempt to widen rights on a derived capability | T2 | `AF_ERR_PERM` |
| `sys_cap_revoke` on a granted frame | T2 | Every derived mapping disappears |
| Four threads, two clients and one server, random traffic | T2 | No lost or duplicated messages under load |

---

## 11. References

* seL4 manual — the reference treatment of capability-based IPC
* QNX Neutrino programmer's guide — message passing in a production microkernel
* Liedtke, *On µ-Kernel Construction* — the canonical argument for this design
* [capability-model.md](capability-model.md) — the rights attached to endpoints
* [memory-model.md](memory-model.md) — what a granted frame actually is

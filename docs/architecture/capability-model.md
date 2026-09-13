# Capability Model

**Status:** 📐 designed · 📐 implemented at v0.7

AfriyieOS has no global namespace. There is no `open("/dev/sda")`, no root user,
no ambient authority. A thread can do exactly what it holds a capability for, and
nothing else.

This is the single design decision that makes hostile code survivable on a
microkernel, and it is the reason drivers can be isolated without also being
trusted.

---

## 1. The problem it solves

In a conventional kernel a process asks for a resource **by name**:

```c
int fd = open("/dev/nvme0n1", O_RDWR);
```

The kernel checks an access-control list and either allows it or not. Every such
check is a place where a bug becomes privilege escalation, and the check must be
repeated at every layer that touches the object.

A capability system replaces the name with an **unforgeable handle**:

```c
af_status_t rc = ipc_call(nvme_endpoint_cap, &request, &reply);
```

The caller never names the device. It either holds a capability to the block
device driver's endpoint or it does not. There is no name to guess, no path to
traverse, no ACL to get wrong, and no second check needed — authority *is* the
handle.

---

## 2. Kernel objects

Every kernel-managed thing a thread can act on is an object:

| Object | Represents |
| --- | --- |
| `AF_OBJECT_THREAD` | A schedulable thread |
| `AF_OBJECT_PROCESS` | An address space, its threads and its capability table |
| `AF_OBJECT_ENDPOINT` | An IPC message queue |
| `AF_OBJECT_REPLY` | A single-use reply slot captured by `ipc_call` |
| `AF_OBJECT_NOTIFICATION` | A bitmask signal/wait object |
| `AF_OBJECT_MEMORY_FRAME` | A physical page |
| `AF_OBJECT_PAGE_TABLE` | A page-table root (an address space) |
| `AF_OBJECT_DEVICE_REGION` | A physical MMIO range |
| `AF_OBJECT_PORT_IO` | An x86 I/O port range |
| `AF_OBJECT_IRQ_HANDLER` | The right to receive a specific interrupt |

Objects are reference counted and destroyed when the last capability to them
disappears.

---

## 3. Capabilities

A capability is a slot index in the holding process's capability table. It is
valid only inside the kernel, so a forged value cannot escape: an index that
names nothing is `AF_ERR_CAP_INVALID`, and an index that names something else is
still bounded by the rights recorded in it.

```c
typedef af_u32 af_cap_t;                  /* slot index; 0 means "empty" */

typedef struct {
    af_object_t *object;                  /* NULL for an empty slot */
    af_u32       rights;                  /* AF_RIGHT_* bitmask      */
    af_u32       generation;              /* detects use of a recycled slot */
} af_cap_slot_t;
```

The `generation` counter closes a real hole: if slot 7 is freed and later
reallocated for a different object, a stale index must fail rather than silently
address the new object. An index the kernel hands out encodes the generation, so
a stale handle is detected rather than misinterpreted.

### 3.1 Rights

| Right | Meaning |
| --- | --- |
| `AF_RIGHT_READ` | Read the object's state |
| `AF_RIGHT_WRITE` | Modify the object's state |
| `AF_RIGHT_EXEC` | Execute code mapped from this frame |
| `AF_RIGHT_GRANT` | Pass this capability to another process |
| `AF_RIGHT_REVOKE` | Revoke capabilities derived from this one |
| `AF_RIGHT_SIGNAL` | Signal the object (notification, endpoint send) |
| `AF_RIGHT_WAIT` | Block on the object (endpoint receive, notification wait) |

The mapping from rights to operations is the *only* place authority is checked,
and it is checked in one function in `kernel/core/cap.c`.

---

## 4. The two rules

> **Rule 1 — derivation may only narrow.**
> `cap_derive(cap, rights)` succeeds only if `rights ⊆ cap.rights`. There is no
> operation anywhere in the system that widens a capability.

> **Rule 2 — revocation is transitive.**
> `cap_revoke(cap)` destroys every capability derived from it, recursively, and
> unmaps every mapping created through one.

Together these give a property that a name-based system cannot offer: **a process
can be fully disarmed.** Revoke the capability it was given, and everything it
built on top of that capability disappears — mappings, derived handles, endpoints
it was handed. It cannot have retained a way back in, because there was never a
name to remember.

This is what makes driver restart genuinely safe.

---

## 5. The derivation tree

```
   init
    │  holds: PROCESS caps for every server,
    │         ENDPOINT caps for the name service
    │
    ├──► name service
    │       │  holds: its own ENDPOINT, and the right to mint
    │       │         read-only copies of itself
    │       └──► grants ENDPOINT(read) to whoever asks
    │
    ├──► device manager
    │       │  holds: DEVICE_REGION + IRQ_HANDLER + PORT_IO caps,
    │       │         granted by the kernel from the boot-time inventory
    │       ├──► nvme driver  : DEVICE_REGION(nvme BAR), IRQ(nvme), ENDPOINT
    │       ├──► usb hid      : DEVICE_REGION(xhci BAR), IRQ(xhci), ENDPOINT
    │       └──► compositor   : FRAME(framebuffer), ENDPOINT
    │
    ├──► file system
    │       └──► holds: ENDPOINT(fs), and ENDPOINT(write) to the block driver
    │
    └──► applications
            └──► hold: their own address space, their window surface,
                      and ENDPOINT(read) to the services they were granted
```

Read the tree downward: **authority only ever flows down, and only in narrowed
form.** A Wi-Fi driver cannot see the disk, not because it is forbidden but
because no path from the Wi-Fi driver leads to the disk's capability.

---

## 6. Bootstrapping

The interesting problem in a capability system: if authority only comes from
holding authority, where does the first capability come from?

There is exactly one privileged moment, and it is the kernel's own init:

1. The kernel creates a **root capability space** holding every object it knows
   about from the boot handoff: the physical memory frames, the framebuffer
   region, the device regions the firmware described.
2. The kernel creates the **init process** — PID 1 — and installs in its table
   only: a capability to the name service endpoint it is about to start, and a
   `PROCESS` capability for each server binary in the ramdisk.
3. The kernel then **drops the root space permanently.** It no longer exists as a
   reachable object.
4. `init` starts the name service, which receives only its own endpoint plus the
   right to hand out read-only copies of it.
5. `init` starts the device manager, granting it the device inventory the kernel
   built at boot.
6. The device manager grants each driver exactly the region, IRQ and port range
   for the one device it drives, then closes its own copies.
7. Every subsequent capability is derived from one of these grants.

After step 3 there is no ambient authority anywhere in the system. The only way
to obtain a capability is to be given one by something that already had it, with
rights no broader than the giver's.

---

## 7. Worked examples

### 7.1 A driver is restarted after crashing

```
1. The NVMe driver faults. The kernel destroys its process.
2. Every capability in its table is released:
     - its DEVICE_REGION  → refcount drops, released
     - its IRQ_HANDLER     → unregistered from the GIC/APIC
     - its ENDPOINT        → destroyed; blocked callers wake with PEER_DEAD
     - every FRAME it was granted → unmapped from its (now dead) address space
3. init sees the process exit, waits a backoff, and starts a new instance.
4. The device manager grants the new instance the same region and IRQ.
5. Callers that got PEER_DEAD retry against the name service.
```

No reboot, no leaked mappings, no half-released device. The system is intact
because the crashed process could not have held anything the kernel did not know
about.

### 7.2 An application cannot read another application's memory

```
1. App A calls ipc_call(fs_cap, {open, "/home/a/notes.txt"}).
2. The file system checks A's capability to itself, not A's identity.
3. It returns a FRAME capability containing the file contents, granted by the
   server, mapped into A's address space only.
4. App B holds no capability to that frame and has no name for it.
   The frame is not addressable from B's page tables at all.
```

There is no path for B to reach it, and no check that could be bypassed — the
data is not in B's address space and B cannot ask for it.

### 7.3 Narrowing on the way down

```
compositor holds:  FRAME(framebuffer), rights READ|WRITE
    │
    └──► grants to an application:
             FRAME(its own window surface), rights READ|WRITE
         and never the framebuffer itself.

    The application draws into its own surface. The compositor composites it.
    A malicious application can therefore corrupt its own window and nothing
    else — it cannot draw over another application, because it has no mapping
    that covers one.
```

---

## 8. What this does *not* protect against

Being honest about the boundaries:

* **A confused deputy.** If a service is granted broad rights and exposes them
  through a narrow interface, a bug in that service is exploitable through the
  narrow interface. Capabilities limit *what can be reached*, not *what a
  reachable service does with it*.
* **Denial of service by resource exhaustion.** Capabilities do not stop a
  process from allocating until memory runs out. That is handled by per-process
  quotas in the PMM and the endpoint queue limits, which are separate mechanisms.
* **Timing side channels.** Not addressed at v1.0.
* **A compromised kernel.** No capability system survives Ring 0. This is why the
  kernel is kept under 64 KB and the syscall surface is small and fuzzed.

---

## 9. Testing

| Test | Tier | What it proves |
| --- | --- | --- |
| Derive with a right the parent lacks | T2 | `AF_ERR_PERM`; widening is impossible |
| Revoke a parent capability | T2 | Every derived capability and mapping disappears |
| Use a stale capability index | T2 | Generation check rejects it |
| Exhaust the capability table | T2 | Clean `AF_ERR_CAP_EXHAUST`, not corruption |
| Process exits holding a granted frame | T2 | Refcount drops; the frame is released |
| Fuzz the syscall surface with random capability values | T2 | No kernel fault; every call returns an error |
| Kill a server with clients blocked on it | T3 | `AF_ERR_PEER_DEAD`, then recovery via init |

---

## 10. References

* seL4 Reference Manual — the design this follows, and the source of Rules 1 and 2
* Shapiro, Smith & Farber, *EROS: a fast capability system*
* Miller, *Robust Composition* — capability security for the working programmer
* [ipc-protocol.md](ipc-protocol.md) — endpoints and message-passing
* [memory-model.md](memory-model.md) — frames and page tables as objects

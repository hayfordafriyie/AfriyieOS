# System Call ABI

**Status:** 🔨 v0.4 — the calling convention and numbers 0–3 and 5 are implemented
and exercised from ring 3. Everything else in §2 is a reserved number that returns
`AF_ERR_NOTSUP` today.

This is the contract between user space and the kernel. It is versioned and never
changes incompatibly without a version bump.

> **As built at v0.4, honestly.** Two things differ from the design below and are
> recorded here rather than quietly corrected:
>
> 1. **The entry instruction is `int 0x80`, not `syscall`.** The register
>    convention is the same, so §1 holds either way and only the stub in §5
>    changes when the instruction does. `syscall`/`sysretq` needs
>    `IA32_STAR`/`LSTAR`/`FMASK` set up and a second entry path, which was not
>    worth adding in the milestone that first crossed the privilege boundary.
> 2. **Argument validation does not yet consult the page tables.** §3 describes
>    `user_ptr_ok` calling `vmm_range_has_access`. At v0.4 it checks range and
>    wraparound and stops there, so a pointer that is in range but *unmapped*
>    faults in kernel mode instead of returning `AF_ERR_FAULT`. This is a real
>    gap, not a simplification — it is the first thing to fix in v0.5.

---

## 1. Calling convention

| | x86_64 | ARM64 |
| --- | --- | --- |
| Instruction | `syscall` / `sysretq` | `svc #0` |
| Number | `rax` | `x8` |
| Arguments | `rdi, rsi, rdx, r10, r8, r9` | `x0, x1, x2, x3, x4, x5` |
| Return | `rax` | `x0` |
| Clobbered | `rcx`, `r11` | — |
| Preserved | everything else | everything else |
| Stack alignment | **16 bytes — mandatory** | 16 bytes |

### 1.1 Return values

Every syscall returns `af_i64`:

* **`>= 0`** — success, with the value's meaning defined per call.
* **`< 0`** — failure, the negated `af_status_t` from
  `kernel/include/afriyie/status.h`.

```c
long rc = af_debug_write("hello", 5);
if (rc < 0) {
    /* rc is -AF_ERR_FAULT, -AF_ERR_INVAL, ... */
}
```

There is no `errno`, no thread-local error slot and no out-parameter convention
for errors. One return register carries the whole answer, which removes a class
of bugs where a caller forgets to check an error output.

### 1.2 The 16-byte alignment rule

On x86_64, `rsp` must be 16-byte aligned at the point of every `call` into
compiled C. Violating it produces crashes inside SSE code paths that appear
random and unrelated to the syscall. The `syscall` entry stub in the kernel
aligns defensively, and user-space stubs in `libaf` maintain the invariant, but it
is worth knowing why: this is the single most time-consuming bug class at v0.4.

---

## 2. The table

| # | Name | Arguments | Returns | From | Implemented |
| --- | --- | --- | --- | --- | --- |
| 0 | `sys_debug_write` | `buf, len` | bytes written | v0.4 | **v0.4 ✓** |
| 1 | `sys_exit` | `code` | *never returns* | v0.4 | **v0.4 ✓** |
| 2 | `sys_thread_create` | `entry, arg, stack, prio` | `cap THREAD` | v0.4 | v0.5 |
| 3 | `sys_thread_yield` | — | 0 | v0.4 | **v0.4 ✓** |
| 4 | `sys_thread_join` | `cap THREAD` | exit code | v0.4 | v0.5 |
| 5 | `sys_clock_get` | `clock_id` | nanoseconds | v0.6 | **v0.4 ✓** *pulled forward* |
| 6 | `sys_sleep` | `ns` | 0 | v0.6 | — |
| 7 | `sys_fb_map` | — | `cap FRAME` + geometry | v0.6 | — |
| 8 | `sys_input_read` | `evt_ptr, max` | events read | v0.5 | — |
| 9 | `sys_dev_claim` | `dev_id` | `cap DEVICE` | v0.5 | — |
| 10 | `sys_ipc_send` | `cap, msg_ptr` | 0 | v0.7 | — |
| 11 | `sys_ipc_recv` | `cap, msg_ptr` | 0 | v0.7 | — |
| 12 | `sys_ipc_call` | `cap, msg_ptr, reply_ptr` | 0 | v0.7 | — |
| 13 | `sys_ipc_reply` | `cap, msg_ptr` | 0 | v0.7 | — |
| 14 | `sys_notify` | `cap, bits` | 0 | v0.8 | — |
| 15 | `sys_wait` | `cap, mask, out_ptr` | observed bits | v0.8 | — |
| 16 | `sys_cap_derive` | `cap, rights` | `cap` | v0.7 | — |
| 17 | `sys_cap_delete` | `cap` | 0 | v0.7 | — |
| 18 | `sys_cap_revoke` | `cap` | 0 | v0.7 | — |
| 19 | `sys_mem_alloc` | `size, flags` | `cap FRAME` | v0.7 | — |
| 20 | `sys_mem_map` | `cap, vaddr, rights` | mapped address | v0.7 | — |
| 21 | `sys_mem_unmap` | `vaddr, size` | 0 | v0.7 | — |
| 22 | `sys_mem_grant` | `dst_proc, cap, vaddr, rights` | target address | v0.7 | — |
| 23 | `sys_irq_wait` | `cap IRQ` | irq number | v0.7 | — |
| 24 | `sys_irq_ack` | `cap IRQ` | 0 | v0.7 | — |
| 25 | `sys_process_spawn` | `path_cap, argv_ptr` | `cap PROCESS` | v0.9 | — |
| 26 | `sys_process_wait` | `cap PROCESS` | exit code | v0.9 | — |

Numbers are **append-only**. A removed call leaves its number reserved forever,
so a stale binary gets `AF_ERR_NOTSUP` rather than reaching a different call.

`sys_clock_get` was scheduled for v0.6 and was implemented at v0.4 instead. The
reason is not convenience: the user program's own acceptance test needed to make a
measurement across a context switch, and at v0.4 the only other observable effect
a program had was printing a line. A clock is what lets a program assert something
about *time* rather than only about output.

---

## 3. Argument validation

**Every pointer argument is validated before it is dereferenced.** This is not
defensive programming; it is the difference between a buggy application and a
kernel compromise.

```c
/* kernel/core/syscall.c */
static bool user_ptr_ok(af_process_t *proc, af_uptr addr, af_size len, bool writable)
{
    /* Reject anything that could wrap around the top of the address space. */
    if (addr + len < addr) {
        return false;
    }
    /* Reject the never-mapped zero page and anything in the kernel half. */
    if (addr < AF_USER_BASE || addr + len > AF_USER_TOP) {
        return false;
    }
    /* Walk the process's own mappings; the range must be fully covered
       and must carry the requested access right. */
    return vmm_range_has_access(proc->space, addr, len, writable);
}
```

Rules, without exception:

1. A user pointer is never dereferenced before `user_ptr_ok` passes.
2. A length from user space is bounds-checked against its buffer before use —
   never trusted, never used to size an allocation without a cap.
3. A capability argument is validated by `cap_lookup`, which checks the slot,
   the generation and the required right.
4. A syscall that can block must have a wakeup condition that includes "the peer
   died" (see [ipc-protocol.md](../architecture/ipc-protocol.md) §8).

The syscall surface is fuzzed from v0.7 with random capability values, random
pointers inside and outside the valid range, and random lengths including `0`,
`SIZE_MAX` and values that would wrap.

---

## 4. Error semantics per call

| Call | Failure modes |
| --- | --- |
| `sys_debug_write` | `AF_ERR_FAULT` (bad buffer), `AF_ERR_INVAL` (len too large) |
| `sys_thread_create` | `AF_ERR_NOMEM`, `AF_ERR_TOOMANY`, `AF_ERR_INVAL` (bad entry/stack) |
| `sys_ipc_call` | `AF_ERR_CAP_INVALID`, `AF_ERR_PERM`, `AF_ERR_PEER_DEAD` |
| `sys_mem_map` | `AF_ERR_NOMEM`, `AF_ERR_INVAL` (address already mapped, or in the kernel half) |
| `sys_mem_grant` | `AF_ERR_PERM` (no `AF_RIGHT_GRANT`), `AF_ERR_CAP_INVALID` (target process gone) |
| `sys_cap_derive` | `AF_ERR_PERM` (attempted to widen), `AF_ERR_CAP_EXHAUST` |
| `sys_process_spawn` | `AF_ERR_NOENT`, `AF_ERR_FS_CORRUPT`, `AF_ERR_NOMEM`, `AF_ERR_TOOMANY` |

`sys_exit` and `sys_thread_join`-on-self never return.

---

## 5. User-space stubs

`libs/libaf` wraps every call so application code never writes inline assembly.
The stub as built at v0.4 (`libs/libaf/include/af.h`) is the three-argument form
the implemented calls need:

```c
/* libs/libaf/include/af.h */
static inline af_i64 af_syscall3(af_u64 number, af_u64 a1, af_u64 a2, af_u64 a3)
{
    af_i64 result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "a"(number), "D"(a1), "S"(a2), "d"(a3)
                         : "memory", "rcx", "r11");
    return result;
}
```

When `SYSCALL` replaces `int 0x80` this becomes `syscall` with the same operands,
and the six-argument form arrives with the first call that needs more than three —
adding it before then would be writing a stub for a calling convention nothing
uses.

Apps then use plain functions, exactly as designed:

```c
long af_debug_write(const char *buf, af_size len);
void af_exit(int code) __attribute__((noreturn));
af_cap_t af_thread_create(void (*entry)(void *), void *arg, af_size stack, int prio);
```

`af_exit` is defined out of line in `libs/libaf/af.c`, not inline in the header:
`crt0.S` calls it by name from assembly, and a `static inline` function is emitted
only if some C translation unit happens to call it — a reference from assembly
counts for nothing, and the link fails.

---

## 6. Versioning

* The **number** of a syscall never changes once released.
* The **meaning** of an argument never changes incompatibly; a new behaviour gets
  a new number.
* The kernel reports its ABI version at boot (`AF_ABI_MAJOR.MINOR`) and it appears
  in the boot log, so a mismatched user space is diagnosable rather than
  mysterious.
* User space is built from the same commit as the kernel during development, so
  a mismatch is a build error rather than a runtime surprise.

---

## 7. References

* [../architecture/ipc-protocol.md](../architecture/ipc-protocol.md) — the message format these calls move
* [../architecture/capability-model.md](../architecture/capability-model.md) — the rights each call checks
* [../architecture/memory-model.md](../architecture/memory-model.md) — what `sys_mem_map` maps
* `kernel/include/afriyie/status.h` — the error codes

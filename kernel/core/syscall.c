// SPDX-License-Identifier: MIT
// AfriyieOS — user mode and system calls
//
// =============================================================================
// WHY int 0x80 AND NOT SYSCALL/SYSRET
// =============================================================================
// SYSCALL/SYSRET is faster: it swaps CS and the stack pointer in hardware from
// MSRs with no descriptor table walk. It is also considerably easier to get
// wrong — STAR/LSTAR/FMASK must all be programmed correctly, SYSRET has a
// documented failure when the return address is non-canonical, and the entry
// path has to save RSP by hand because SYSCALL does not record it anywhere.
//
// int 0x80 costs a descriptor table lookup and goes through the same ISR
// machinery every other interrupt already uses. That means the stack switch
// through TSS.RSP0 is handled by hardware, the register save is already written
// and tested, and the failure modes are ones this kernel already understands.
//
// So: correctness first, and SYSCALL/SYSRET becomes an optimisation once there
// is a working baseline to measure it against. The syscall ABI does not change —
// only the instruction the user program executes.
// =============================================================================

#include "afriyie/hal.h"
#include "afriyie/types.h"
#include "afriyie/status.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/process.h"
#include "afriyie/heap.h"
#include "afriyie/pmm.h"
#include "afriyie/config.h"
#include "afriyie/syscall.h"

#if AF_TARGET_X86_64
#include "../arch/x86_64/x86_64.h"
#endif

// -----------------------------------------------------------------------------
// System call numbers
//
// These match docs/abi/syscalls.md. The numbering is append-only: a removed call
// leaves its number reserved forever so that a stale binary gets AF_ERR_NOTSUP
// rather than reaching a different call.
// -----------------------------------------------------------------------------
#define AF_SYS_DEBUG_WRITE   0
#define AF_SYS_EXIT          1
#define AF_SYS_YIELD         3
#define AF_SYS_CLOCK_GET     5
#define AF_SYS_MAX           64

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static af_u64 s_syscall_count = 0;
static af_u64 s_syscall_by_number[AF_SYS_MAX];

// AF_USER_OK is a one-shot marker. See the comment at its emission site.
static bool s_user_ok_emitted = false;

af_u64 usermode_syscall_count(void)
{
    return s_syscall_count;
}

// -----------------------------------------------------------------------------
// Argument validation
//
// =============================================================================
// A USER POINTER IS CHECKED BEFORE IT IS DEREFERENCED. ALWAYS.
// =============================================================================
// This is the difference between a buggy application and a compromised kernel.
// The first version of this kernel had no user-mode code, so there was nothing
// to validate against; from this point on, every pointer arriving through a
// system call is hostile until proven otherwise.
//
// The checks are deliberately conservative:
//
//   * the whole range must be below the kernel's half of the address space
//   * wrapping past the top of memory is rejected by testing the END, not the
//     start — a length that overflows is the classic way past a bounds check
//   * the null page is never mapped, so an address near zero is refused
//   * and every page the range touches must actually BE MAPPED, and carry the
//     USER bit, and be writable if the call is going to write to it
//
// The last of those is the one that matters most, and it was missing until now.
// A range check answers "is this address in the user half?"; it does not answer
// "does this address exist?". Without the second question, a program passing a
// pointer into a hole between two of its own mappings reached the kernel's copy
// and took a page fault in kernel mode. The fault handler reports it as a bug in
// the kernel, which is exactly what it is — the kernel dereferenced a pointer it
// had been told to trust.
//
// Walking the range costs a page-table lookup per page, and that is a cost a
// program gets to choose. So the walk is bounded: see AF_MAX_VALIDATED_PAGES.
// =============================================================================

// The largest range a single call may have validated, in pages (16 MiB).
//
// A user-supplied length is attacker-controlled, and walking an arbitrarily long
// range one page at a time is a denial-of-service vector against the kernel. A
// call that genuinely needs to name more than this must move the data in
// chunks, or describe the mapping rather than the buffer.
#define AF_MAX_VALIDATED_PAGES 4096

static bool user_range_ok(af_u64 addr, af_u64 length)
{
    // A zero-length buffer is meaningless and almost certainly a caller bug.
    if (length == 0) {
        return false;
    }

    // Address zero is never mapped; treat anything in the first page as null.
    if (addr < AF_USER_BASE) {
        return false;
    }

    // Test the END for wraparound. Checking only the start lets a length of
    // 0xFFFFFFFFFFFFFFFF pass with any address at all.
    af_u64 end = addr + length;
    if (end < addr) {
        return false;
    }

    // The whole range must be in the user half.
    if (end > AF_USER_TOP) {
        return false;
    }

    return true;
}

// True when every page the range touches is mapped, user-accessible, and
// writable if the caller intends to write.
//
// The first and last addresses are rounded outwards, so a range that starts or
// ends mid-page is validated over the whole of the pages it sits in rather than
// only the bytes it names. A program that passes the last three bytes of a
// mapped page and a length that spills into the next one must not be able to
// reach a page it does not own by starting inside a page it does.
static bool user_pages_ok(af_u64 addr, af_u64 length, bool needs_write)
{
    const af_u64 pages = ((addr & AF_PAGE_MASK) + length + AF_PAGE_MASK)
                         / AF_PAGE_SIZE;
    if (pages == 0 || pages > AF_MAX_VALIDATED_PAGES) {
        return false;
    }

    hal_pt_root_t root = hal_get_page_table();
    af_u64 page = addr & ~(af_u64)AF_PAGE_MASK;

    for (af_u64 i = 0; i < pages; i++, page += AF_PAGE_SIZE) {
        const af_u32 flags = hal_query_flags(root, (af_vaddr)page);

        if ((flags & HAL_PRESENT) == 0) {
            return false;
        }

        // Mapped but supervisor-only is not good enough. Ring 3 could not have
        // read it either, and the only pages in that state belong to the kernel.
        if ((flags & HAL_USER) == 0) {
            return false;
        }

        if (needs_write && (flags & HAL_WRITABLE) == 0) {
            return false;
        }
    }

    return true;
}

// The check every system call that takes a buffer runs.
static bool user_buffer_ok(af_u64 addr, af_u64 length, bool needs_write)
{
    return user_range_ok(addr, length) &&
           user_pages_ok(addr, length, needs_write);
}


// -----------------------------------------------------------------------------
// Calls
// -----------------------------------------------------------------------------
static af_i64 sys_debug_write(af_u64 buf, af_u64 len)
{
    // The cap comes FIRST, so validation walks only the pages that will actually
    // be read. A program that offers a 1 MiB buffer has it truncated to one line
    // and only that line's pages are checked — which is both cheaper and more
    // truthful: the kernel is not reading the rest, so it has no business
    // requiring it to be valid.
    //
    // A hard cap as well as the range check: a program may legitimately hold a
    // very large buffer, but a single log line never needs to be one.
    if (len > 4096) {
        len = 4096;
    }

    if (!user_buffer_ok(buf, len, false)) {
        af_warn("syscall", "sys_debug_write: rejected buffer 0x%lX length %llu "
                           "(out of the user range, wrapping, or not mapped "
                           "user-readable)",
                buf, (unsigned long long)len);
        return AF_ERR_FAULT;
    }

    const char *text = (const char *)(af_uptr)buf;

    // Write it in one go, through the kernel's own console. At v0.7 this becomes
    // a message to the terminal service over IPC; the ABI does not change.
    af_log_raw_n(text, (af_size)len);

    return (af_i64)len;
}

static af_i64 sys_clock_get(af_u64 clock_id)
{
    AF_UNUSED(clock_id);
    return (af_i64)hal_time_ns();
}

// -----------------------------------------------------------------------------
// Dispatch
//
// Returns the value to place in RAX. Never returns for sys_exit.
// -----------------------------------------------------------------------------
static af_i64 syscall_dispatch(af_u64 number, af_u64 a1, af_u64 a2, af_u64 a3,
                               af_u64 a4, af_u64 a5)
{
    AF_UNUSED(a3);
    AF_UNUSED(a4);
    AF_UNUSED(a5);

    s_syscall_count++;
    if (number < AF_SYS_MAX) {
        s_syscall_by_number[number]++;
    }

    switch (number) {
    case AF_SYS_DEBUG_WRITE:
        return sys_debug_write(a1, a2);

    case AF_SYS_CLOCK_GET:
        return sys_clock_get(a1);

    case AF_SYS_YIELD:
        sched_yield();
        return AF_OK;

    case AF_SYS_EXIT: {
        af_info("syscall", "user thread exiting with code %llu",
                (unsigned long long)a1);

        // Reaching here at all is the acceptance evidence: a ring-3 program ran,
        // made system calls, and asked to terminate. The marker is emitted here
        // rather than from the test's own code because the test never regains
        // control — its final act is this call.
        //
        // Emitted once, for the FIRST user thread to exit. Two ring-3 contexts
        // run at boot now: the built-in stub, then init loaded off the disk.
        // Without this guard the second exit would print AF_USER_OK again, and
        // the boot log would suggest the stub had run twice rather than that
        // two different tests had each passed. init's own exit is evidenced by
        // AF_EXEC_RAN, which its code prints before calling this.
        if (s_syscall_count >= 3 && !s_user_ok_emitted) {
            s_user_ok_emitted = true;
            af_marker("AF_USER_OK");
            af_info("syscall", "%llu system calls handled from ring 3",
                    (unsigned long long)s_syscall_count);
        }

        // The ring-3 context belongs to a thread; terminating it means the
        // scheduler never returns to the user frame, so the iretq that would
        // have resumed ring 3 simply does not happen.
        //
        // THIS GOES THROUGH process_exit, and it did not used to. The original
        // version marked the thread a zombie by hand and called thread_exit
        // directly, which terminates the THREAD and leaves the PROCESS alive —
        // so init exited, its thread was reaped, and its process sat in the
        // table forever holding an address space and a pid.
        //
        // The leak was invisible on every diagnostic that existed: the boot test
        // passed, no marker was missing, and the only symptom was a process that
        // was never released. It was found by reading the boot log for a
        // "reaping pid" line that should have been there after init exited and
        // was not.
        //
        // process_exit sets the process's exit code, marks it a zombie, wakes a
        // parent blocked in process_wait, and then terminates this thread. A
        // thread with no process — the ring-3 stub — falls through to the same
        // thread_exit, because process_exit handles that case rather than
        // requiring it to be checked here.
        process_exit((af_i32)a1);
    }

    default:
        af_warn("syscall", "unimplemented system call %llu from ring 3",
                (unsigned long long)number);
        return AF_ERR_NOTSUP;
    }
}

// -----------------------------------------------------------------------------
// Entry point
//
// Called from the interrupt dispatcher for vector 0x80. The frame is the one
// isr_common built, and it is complete because the CPU switched stacks through
// TSS.RSP0 on the way in.
//
// The parameter is `void *` in the header because syscall.h is included by
// architecture-independent code and isr_frame_t is an x86_64 structure. Casting
// here keeps the architecture-specific type out of the shared interface, which
// is the same reason the HAL exists.
// -----------------------------------------------------------------------------
void syscall_entry(void *frame_opaque)
{
    isr_frame_t *frame = (isr_frame_t *)frame_opaque;

    if (frame == NULL) {
        return;
    }

    // Register convention, matching docs/abi/syscalls.md:
    //   rax = number, rdi/rsi/rdx/r10/r8/r9 = arguments, rax = result
    //
    // int 0x80 carries them the same way SYSCALL would, so the user-side stub
    // does not change when the instruction is upgraded later.
    af_i64 result = syscall_dispatch(frame->rax, frame->rdi, frame->rsi,
                                     frame->rdx, frame->r10, frame->r8);

    // The frame is written back, and isr_common restores it on the way out — so
    // the value the user program reads from RAX is this one.
    frame->rax = (af_u64)result;
}

void usermode_dump_stats(void)
{
    af_info("syscall", "%llu system calls in %u distinct numbers",
            (unsigned long long)s_syscall_count, (af_u32)AF_SYS_MAX);

    for (af_u32 i = 0; i < AF_SYS_MAX; i++) {
        if (s_syscall_by_number[i] != 0) {
            af_info("syscall", "  syscall %-2u: %llu calls", i,
                    (unsigned long long)s_syscall_by_number[i]);
        }
    }
}

// =============================================================================
// The v0.4 acceptance test
// =============================================================================
//
// Builds a ring-3 program in a user-accessible page, drops to ring 3, and lets
// it make system calls. There is no ELF loader yet, so the program is assembled
// by hand into the page — which is honest about what is being tested. This test
// is about the PRIVILEGE BOUNDARY, not about loading executables.
//
// The addresses are deliberately ABOVE the identity map (which covers the low
// 3 GiB as kernel-only). Mapping them at 4 GiB means:
//
//   * the page-table walk is genuinely exercised rather than landing in a huge
//     page, and
//   * the user pages carry the USER bit and the identity map does not, so ring 3
//     genuinely cannot reach kernel memory.
//
// A program that ran in pages the kernel had already mapped for itself would
// prove nothing about isolation.
//
// Inside the user region — PML4[1], 512 GiB — and not the low 4 GiB, because
// PML4[0] is where the kernel's identity map lives and the two must not share a
// top-level entry. See AF_USER_PML4_INDEX in config.h for why that is a
// correctness requirement and not a preference.
//
// The stub and the ELF-loader programs can now share this address, because they
// no longer share an address SPACE: the stub runs in the kernel's, init in its
// own. They previously collided at 4 GiB and the loader failed with ERR_EXIST.
// =============================================================================

#define USER_CODE_BASE   AF_USER_REGION_BASE        // 512 GiB
// Page-aligned, and the addition is a multiple of the page size on purpose: an
// offset of one KiB puts the stack base at 0x8000000400 and hal_map_page rejects
// it with ERR_INVAL, which is a correct refusal and a confusing symptom.
#define USER_STACK_BASE  (AF_USER_REGION_BASE + (16 * AF_PAGE_SIZE))
#define USER_MESSAGE_OFFSET 128

void usermode_selftest(void)
{
    af_info("test", "user mode test: building a ring-3 program at 0x%lX",
            (af_u64)USER_CODE_BASE);

    hal_pt_root_t root = hal_get_page_table();

    // --- map the user code page and stack ------------------------------------
    af_paddr code_frame  = pmm_alloc_frame_z();
    af_paddr stack_frame = pmm_alloc_frame_z();

    if (code_frame == AF_FRAME_INVALID || stack_frame == AF_FRAME_INVALID) {
        af_panic("user mode test: could not allocate user pages");
    }

    // The code page is USER + EXEC but NOT writable, and the stack is USER +
    // writable but NOT executable. The NX bit is what makes the difference: a
    // stack that can be executed is how a buffer overflow becomes code
    // execution, and it costs nothing to prevent.
    af_status_t rc = hal_map_page(root, USER_CODE_BASE, code_frame,
                                  HAL_PRESENT | HAL_USER | HAL_EXEC);
    if (af_status_err(rc)) {
        af_panic("user mode test: could not map the user code page (%s)",
                 af_status_name(rc));
    }

    rc = hal_map_page(root, USER_STACK_BASE, stack_frame,
                      HAL_PRESENT | HAL_WRITABLE | HAL_USER);
    if (af_status_err(rc)) {
        af_panic("user mode test: could not map the user stack (%s)",
                 af_status_name(rc));
    }

    // --- build the program ----------------------------------------------------
    //
    // Written through the identity map, because the user page is read-only from
    // ring 3 and we are in ring 0 with the page tables in hand. The mapping is
    // not writable to the user, so the program cannot modify itself once
    // running.
    //
    // The kernel can still write it because the kernel's access does not go
    // through the USER bit — the page-table entry says what ring 3 may do, not
    // what ring 0 may do.
    af_u8 *code = (af_u8 *)(af_uptr)code_frame;

    const char *message = "Hello from ring 3!\n";
    af_size message_length = af_strlen(message);

    af_memcpy(code + USER_MESSAGE_OFFSET, message, message_length + 1);

    af_u64 message_address = USER_CODE_BASE + USER_MESSAGE_OFFSET;
    af_u32 p = 0;

    // mov rax, 0        ; sys_debug_write
    code[p++] = 0x48; code[p++] = 0xC7; code[p++] = 0xC0; code[p++] = 0x00; code[p++] = 0x00; code[p++] = 0x00; code[p++] = 0x00;
    // mov rdi, <message>
    code[p++] = 0x48; code[p++] = 0xBF;
    af_memcpy(code + p, &message_address, 8); p += 8;
    // mov rsi, <length>
    code[p++] = 0x48; code[p++] = 0xC7; code[p++] = 0xC6;
    af_memcpy(code + p, &message_length, 4); p += 4;
    // int 0x80
    code[p++] = 0xCD; code[p++] = 0x80;

    // A SECOND call, deliberately: the first proves the transition into ring 3
    // works, and the second proves it works again after a return through the
    // interrupt path. A transition that only survives one call has a broken
    // return, which is the more common failure and the harder one to see.
    //
    // mov rax, 0
    code[p++] = 0x48; code[p++] = 0xC7; code[p++] = 0xC0; code[p++] = 0x00; code[p++] = 0x00; code[p++] = 0x00; code[p++] = 0x00;
    // mov rdi, <message>
    code[p++] = 0x48; code[p++] = 0xBF;
    af_memcpy(code + p, &message_address, 8); p += 8;
    // mov rsi, <length>
    code[p++] = 0x48; code[p++] = 0xC7; code[p++] = 0xC6;
    af_memcpy(code + p, &message_length, 4); p += 4;
    // int 0x80
    code[p++] = 0xCD; code[p++] = 0x80;

    // mov rax, 1        ; sys_exit
    code[p++] = 0x48; code[p++] = 0xC7; code[p++] = 0xC0; code[p++] = 0x01; code[p++] = 0x00; code[p++] = 0x00; code[p++] = 0x00;
    // xor rdi, rdi      ; exit code 0
    code[p++] = 0x48; code[p++] = 0x31; code[p++] = 0xFF;
    // int 0x80
    code[p++] = 0xCD; code[p++] = 0x80;
    // hlt, in case exit ever returns — which it must not, but a user program
    // that falls off the end of its own code is a real failure mode.
    code[p++] = 0xF4;

    if (p > USER_MESSAGE_OFFSET) {
        af_panic("user mode test: the program is %u bytes and would overlap "
                 "its own message at offset %u", p, (af_u32)USER_MESSAGE_OFFSET);
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   built a %u-byte ring-3 program; "
                                  "message at 0x%lX", p, message_address);

    // --- verify the boundary before crossing it ------------------------------
    //
    // Both pages must be marked USER. If the USER bit were missing the
    // transition would fault on the first instruction fetch, and the fault would
    // be reported as a page fault at the entry address — which looks like a bad
    // entry point rather than a missing permission bit.
    af_u32 code_flags = hal_query_flags(root, USER_CODE_BASE);
    af_u32 stack_flags = hal_query_flags(root, USER_STACK_BASE);

    if ((code_flags & HAL_USER) == 0) {
        af_panic("user mode test: the code page is not user-accessible "
                 "(flags 0x%X)", code_flags);
    }
    if ((code_flags & HAL_EXEC) == 0) {
        af_panic("user mode test: the code page is not executable "
                 "(flags 0x%X)", code_flags);
    }
    if ((code_flags & HAL_WRITABLE) != 0) {
        af_panic("user mode test: the code page is writable from ring 3, so the "
                 "program could modify itself");
    }
    if ((stack_flags & HAL_USER) == 0) {
        af_panic("user mode test: the stack is not user-accessible "
                 "(flags 0x%X)", stack_flags);
    }
    if ((stack_flags & HAL_EXEC) != 0) {
        af_panic("user mode test: the stack is executable, which turns a buffer "
                 "overflow into code execution");
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   code page is user+exec and NOT writable; "
                                  "stack is user+writable and NOT executable");

    // The kernel's own memory must NOT be reachable from ring 3. Every page in
    // the identity map was created without the USER bit, so this checks the
    // property that makes the boundary real rather than the one that makes the
    // program run.
    af_u32 kernel_flags = hal_query_flags(root, 0x100000);
    if ((kernel_flags & HAL_USER) != 0) {
        af_panic("user mode test: the kernel image at 0x100000 is "
                 "user-accessible (flags 0x%X) — ring 3 could read and write "
                 "kernel memory", kernel_flags);
    }

    af_log(AF_LOG_DEBUG, "test", "  ok   the kernel image at 0x100000 is NOT "
                                  "user-accessible");

    af_marker("AF_USER_PREPARED");

    // --- cross the boundary ---------------------------------------------------
    af_info("test", "  crossing into ring 3 at 0x%lX with stack 0x%lX",
            (af_u64)USER_CODE_BASE, (af_u64)(USER_STACK_BASE + AF_PAGE_SIZE));
    af_info("test", "  ----------------------------------------------------------");

    // Never returns: the program's final system call is sys_exit, which
    // terminates the thread this is running on rather than resuming it.
    af_x86_enter_user_mode(USER_CODE_BASE, USER_STACK_BASE + AF_PAGE_SIZE);
}

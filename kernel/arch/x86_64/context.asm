; SPDX-License-Identifier: MIT
; AfriyieOS — x86_64 thread context switch
;
;   void context_switch(af_context_t *old, af_context_t *new);
;     rdi = the context to save into
;     rsi = the context to resume
;
; =============================================================================
; HOW A BRAND NEW THREAD STARTS
; =============================================================================
; The switch ends with `ret`, which pops the address on top of the *incoming*
; thread's stack. For a thread that has never run, thread_create() sets that
; address to thread_trampoline and puts the entry function in r12 and its
; argument in r13 — both of which this code restores before the ret.
;
; So the first switch into a new thread lands in thread_trampoline, which moves
; r13 into the first argument register and calls r12. No special case is needed
; anywhere in the scheduler: a new thread and a resumed thread are the same
; thing to this function.
;
; =============================================================================
; FIELD OFFSETS ARE THE C STRUCT CONTRACT
; =============================================================================
; These offsets must match af_context_t in kernel/include/afriyie/thread.h, which
; asserts them on the C side. A mismatch compiles perfectly and produces a thread
; that resumes with one wrong register — a fault that appears thousands of
; switches later, in code that has nothing to do with the scheduler.
; =============================================================================

[bits 64]

section .text

extern thread_exit_from_asm

global context_switch
global thread_trampoline

; Offsets into af_context_t
%define CTX_RBX 0
%define CTX_RBP 8
%define CTX_R12 16
%define CTX_R13 24
%define CTX_R14 32
%define CTX_R15 40
%define CTX_RSP 48

; -----------------------------------------------------------------------------
; context_switch
; -----------------------------------------------------------------------------
context_switch:
    ; --- save the outgoing context ---
    mov [rdi + CTX_RBX], rbx
    mov [rdi + CTX_RBP], rbp
    mov [rdi + CTX_R12], r12
    mov [rdi + CTX_R13], r13
    mov [rdi + CTX_R14], r14
    mov [rdi + CTX_R15], r15

    ; =========================================================================
    ; SAVE rsp POINTING *AT* THE RETURN ADDRESS, NOT PAST IT
    ; =========================================================================
    ; This is the single most delicate line in the kernel, and the first version
    ; got it wrong with a plausible-sounding justification.
    ;
    ; The mistake: "our return address will not be popped by the `ret` below, so
    ; record rsp after it" — hence `lea rax, [rsp + 8]`. That skips eight bytes,
    ; and when this thread is resumed, `mov rsp, [ctx+CTX_RSP]` followed by `ret`
    ; pops whatever happens to sit at that slot instead of the real return
    ; address. The CPU then jumps to garbage.
    ;
    ; The observed failure was an invalid-opcode fault with RIP inside .bss —
    ; a control transfer to a data address — with a stack belonging to one of the
    ; worker threads. Nothing looked wrong; the scheduler simply resumed into the
    ; middle of nowhere a few hundred switches in.
    ;
    ; The correct convention is the one a `call`/`ret` pair already uses: rsp
    ; points at the return address. Saving rsp unchanged means the outgoing
    ; thread's return address stays on ITS stack, and the `ret` below consumes
    ; the INCOMING thread's. Both cases then work with no arithmetic at all —
    ; including a brand new thread, whose stack has thread_trampoline sitting in
    ; exactly that slot.
    ; =========================================================================
    mov [rdi + CTX_RSP], rsp

    ; --- restore the incoming context ---
    mov rbx, [rsi + CTX_RBX]
    mov rbp, [rsi + CTX_RBP]
    mov r12, [rsi + CTX_R12]
    mov r13, [rsi + CTX_R13]
    mov r14, [rsi + CTX_R14]
    mov r15, [rsi + CTX_R15]
    mov rsp, [rsi + CTX_RSP]

    ; For a resumed thread this returns to the instruction after its own call.
    ; For a new thread it pops thread_trampoline and starts it.
    ret

; -----------------------------------------------------------------------------
; thread_trampoline
;
; Entry point for a thread that has never run. Reached by the `ret` above, with
; r12 = the entry function and r13 = its argument.
;
; At this point rsp is 8 mod 16, which is exactly what the SysV ABI requires at
; a function's first instruction — the `ret` that reached us did the same thing
; a `call` would have.
; -----------------------------------------------------------------------------
thread_trampoline:
    ; A brand new thread may be entered from inside the timer interrupt, where
    ; the interrupt gate leaves IF cleared. Unlike a resumed thread — which
    ; returns through its own `iretq` and gets its flags back — this thread has
    ; no earlier state to restore, so it must enable interrupts itself.
    ;
    ; Without this, the first thread created after preemption begins runs with
    ; interrupts off forever: no ticks, no further preemption, and a machine
    ; that appears to hang the moment a second thread starts.
    sti

    ; The System V ABI puts the first argument in rdi. r12 and r13 are
    ; callee-saved, so they still hold what thread_create() put there.
    mov rdi, r13
    call r12

    ; The entry function returned. A thread that falls off the end of its
    ; function must still terminate — otherwise the CPU executes whatever
    ; follows in memory with a live stack.
    call thread_exit_from_asm

    ; thread_exit_from_asm never returns. If it somehow does, stop the CPU.
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits

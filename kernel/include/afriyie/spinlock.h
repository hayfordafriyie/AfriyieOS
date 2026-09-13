// SPDX-License-Identifier: MIT
// AfriyieOS — spinlocks

#ifndef AFRIYIE_SPINLOCK_H
#define AFRIYIE_SPINLOCK_H

#include "types.h"
#include "io.h"

// A test-and-set spinlock with interrupt state captured alongside.
//
// The interrupt flag is stored IN the lock rather than being a separate concern,
// because the only correct way to take a lock in a kernel that also has
// interrupt handlers is to disable interrupts first. Splitting the two invites
// the classic deadlock: a thread takes the lock, an interrupt arrives on the
// same CPU, the handler tries the same lock, and the CPU spins forever waiting
// for itself.
typedef struct {
    volatile af_u32 locked;
    af_u64         saved_flags;
} af_spinlock_t;

#define AF_SPINLOCK_INIT { 0, 0 }

AF_INLINE void af_spin_lock(af_spinlock_t *lock)
{
    lock->saved_flags = af_save_flags_and_disable_interrupts();

    while (__atomic_test_and_set((volatile void *)&lock->locked,
                                 __ATOMIC_ACQUIRE)) {
        // Spin. The `pause` instruction is not decoration: it desynchronises the
        // pipeline's speculative load loop, cuts power on hyperthreaded cores,
        // and measurably speeds up the release on the other side.
#if AF_TARGET_X86_64
        __asm__ __volatile__("pause");
#endif
    }
}

AF_INLINE void af_spin_unlock(af_spinlock_t *lock)
{
    __atomic_clear((volatile void *)&lock->locked, __ATOMIC_RELEASE);
    af_restore_flags(lock->saved_flags);
}

AF_INLINE bool af_spin_trylock(af_spinlock_t *lock)
{
    af_u64 flags = af_save_flags_and_disable_interrupts();

    if (__atomic_test_and_set((volatile void *)&lock->locked,
                              __ATOMIC_ACQUIRE)) {
        af_restore_flags(flags);
        return false;
    }

    lock->saved_flags = flags;
    return true;
}

#endif // AFRIYIE_SPINLOCK_H

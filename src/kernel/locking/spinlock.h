/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_SPINLOCK_H
#define KERNEL_LOCKING_SPINLOCK_H

#include <lib/compiler.h>

#include <hal/irq.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    union {
        volatile uint32_t val;
        struct {
            volatile uint8_t locked;
            uint8_t reserved;
            volatile uint16_t tail;
        };
    };
} spinlock_t;

#define SPINLOCK_LOCKED_VAL 1u

/* Out-of-line slow path, implemented in spinlock.c */
void spinlock_acquire_slowpath(spinlock_t* lock);

__attribute__((always_inline)) static inline void spinlock_init(spinlock_t* lock) {
#ifdef __cplusplus
    if (kernel::unlikely(!lock)) {
#else
    if (unlikely(!lock)) {
#endif
        return;
    }

    lock->val = 0u;
}

__attribute__((always_inline)) static inline void spinlock_acquire(spinlock_t* lock) {
    uint32_t expected = 0u;

    /*
     * Fast path: the lock is completely free (locked == 0, tail == 0).
     * One single atomic instruction to grab it.
     */
    const int acquired = __atomic_compare_exchange_n(
        &lock->val, &expected, SPINLOCK_LOCKED_VAL,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
    );

#ifdef __cplusplus
    if (kernel::likely(acquired)) {
#else
    if (likely(acquired)) {
#endif
        return;
    }

    /*
     * Slow path: lock is contended. Move to the out-of-line handler
     * to avoid bloating the instruction cache.
     */
    spinlock_acquire_slowpath(lock);
}

__attribute__((always_inline)) static inline int spinlock_try_acquire(spinlock_t* lock) {
    uint32_t expected = 0u;

    const int acquired = __atomic_compare_exchange_n(
        &lock->val, &expected, SPINLOCK_LOCKED_VAL,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
    );

    return acquired ? 1 : 0;
}

__attribute__((always_inline)) static inline void spinlock_release(spinlock_t* lock) {
    /*
     * Extremely fast release.
     * On x86, an aligned byte store has release semantics by default.
     * This compiles down to a single 'mov byte ptr [eax], 0' instruction,
     * completely avoiding the heavy 'lock' prefix.
     */
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
}

__attribute__((always_inline)) static inline uint32_t spinlock_acquire_safe(spinlock_t* lock) {
    uint32_t flags = irq_save();

    spinlock_acquire(lock);

    return flags;
}

__attribute__((always_inline)) static inline void spinlock_release_safe(spinlock_t* lock, uint32_t flags) {
    spinlock_release(lock);

    if (flags & 0x200u) {
        irq_enable();
    }
}

#ifdef __cplusplus
}
#endif

#endif
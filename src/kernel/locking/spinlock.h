// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_LOCKING_SPINLOCK_H
#define KERNEL_LOCKING_SPINLOCK_H

#include <stdint.h>

#include <kernel/smp/cpu_limits.h>
#include <kernel/panic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t next;
    volatile uint32_t locked;
} spinlock_qnode_t;

#define SPINLOCK_QNODE_DEPTH 128

extern spinlock_qnode_t g_spinlock_qnodes[MAX_CPUS][SPINLOCK_QNODE_DEPTH];
extern volatile uint32_t g_spinlock_qnode_tops[MAX_CPUS];

typedef struct {
    volatile uint32_t tail;
} spinlock_t;

static inline void spinlock_init(spinlock_t* lock) {
    lock->tail = 0u;
}

static inline void spinlock_qnode_cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

int hal_cpu_index(void);

static inline spinlock_qnode_t* spinlock_qnode_alloc(void) {
    const int cpu = hal_cpu_index();

    uint32_t idx = __atomic_fetch_add(&g_spinlock_qnode_tops[cpu], 1u, __ATOMIC_RELAXED);
    if (idx >= SPINLOCK_QNODE_DEPTH) {
        kernel_panic("spinlock qnode depth overflow", "spinlock.h", __LINE__, 0);
    }

    return &g_spinlock_qnodes[cpu][idx];
}

static inline spinlock_qnode_t* spinlock_qnode_current(void) {
    const int cpu = hal_cpu_index();
    uint32_t idx = __atomic_load_n(&g_spinlock_qnode_tops[cpu], __ATOMIC_RELAXED);

    if (idx == 0u) {
        kernel_panic("spinlock qnode underflow", "spinlock.h", __LINE__, 0);
    }

    return &g_spinlock_qnodes[cpu][idx - 1u];
}

static inline void spinlock_qnode_free(void) {
    const int cpu = hal_cpu_index();
    (void)__atomic_fetch_sub(&g_spinlock_qnode_tops[cpu], 1u, __ATOMIC_RELAXED);
}

static inline void spinlock_acquire(spinlock_t* lock) {
    spinlock_qnode_t* node = spinlock_qnode_alloc();

    __atomic_store_n(&node->next, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&node->locked, 1u, __ATOMIC_RELAXED);

    const uint32_t me = (uint32_t)(uintptr_t)node;

    uint32_t prev = __atomic_exchange_n(&lock->tail, me, __ATOMIC_ACQ_REL);
    if (prev == 0u) {
        __atomic_store_n(&node->locked, 0u, __ATOMIC_RELAXED);
        return;
    }

    spinlock_qnode_t* pred = (spinlock_qnode_t*)(uintptr_t)prev;
    __atomic_store_n(&pred->next, me, __ATOMIC_RELEASE);

    while (__atomic_load_n(&node->locked, __ATOMIC_ACQUIRE) != 0u) {
        spinlock_qnode_cpu_relax();
    }
}

static inline uint32_t spinlock_acquire_safe(spinlock_t* lock) {
    uint32_t flags;

    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    spinlock_acquire(lock);

    return flags;
}

static inline int spinlock_try_acquire(spinlock_t* lock) {
    spinlock_qnode_t* node = spinlock_qnode_alloc();

    __atomic_store_n(&node->next, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&node->locked, 0u, __ATOMIC_RELAXED);

    const uint32_t me = (uint32_t)(uintptr_t)node;

    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(
            &lock->tail,
            &expected,
            me,
            0,
            __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED
        )) {
        return 1;
    }

    spinlock_qnode_free();
    return 0;
}

static inline void spinlock_release(spinlock_t* lock) {
    spinlock_qnode_t* node = spinlock_qnode_current();
    const uint32_t me = (uint32_t)(uintptr_t)node;

    uint32_t succ = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE);
    if (succ == 0u) {
        uint32_t expected = me;
        if (__atomic_compare_exchange_n(
                &lock->tail,
                &expected,
                0u,
                0,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED
            )) {
            spinlock_qnode_free();
            return;
        }

        do {
            spinlock_qnode_cpu_relax();
            succ = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE);
        } while (succ == 0u);
    }

    spinlock_qnode_t* next = (spinlock_qnode_t*)(uintptr_t)succ;
    __atomic_store_n(&next->locked, 0u, __ATOMIC_RELEASE);

    spinlock_qnode_free();
}

static inline void spinlock_release_safe(spinlock_t* lock, uint32_t flags) {
    spinlock_release(lock);

    if (flags & 0x200) {
        __asm__ volatile("sti");
    }
}

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/smp/cpu_limits.h>
#include <kernel/panic.h>

#include <hal/cpu.h>

#include <stddef.h>

#include "spinlock.h"

/*
 * The depth is strictly bounded by the maximum number of interrupt nesting
 * levels (e.g., Thread -> SoftIRQ -> HardIRQ -> NMI). 4 is sufficient.
 */
#define QNODE_DEPTH 4u

typedef struct qnode {
    volatile struct qnode* next;

    volatile uint8_t locked;
    uint8_t padding[3];
} qnode_t;

static qnode_t g_qnodes[MAX_CPUS][QNODE_DEPTH];
static uint32_t g_qnode_idx[MAX_CPUS];

__attribute__((always_inline)) static inline uint32_t encode_tail(int cpu, uint32_t idx) {
    /*
     * Bits 16-29: CPU index + 1 (so CPU 0 is not treated as an empty tail).
     * Bits 30-31: Node index (nesting level).
     */
    const uint32_t cpu_part = (uint32_t)cpu + 1u;
    const uint32_t tail_16 = cpu_part | (idx << 14u);

    return tail_16 << 16u;
}

__attribute__((always_inline)) static inline qnode_t* decode_tail(uint32_t val) {
    const uint32_t tail_16 = val >> 16u;
    
    const uint32_t cpu = (tail_16 & 0x3FFFu) - 1u;
    const uint32_t idx = tail_16 >> 14u;

    if (unlikely(cpu >= MAX_CPUS || idx >= QNODE_DEPTH)) {
        panic("SPINLOCK: corrupted tail decoded");
    }

    return &g_qnodes[cpu][idx];
}

void spinlock_acquire_slowpath(spinlock_t* lock) {
    const int cpu = hal_cpu_index();

    if (unlikely(cpu < 0 || cpu >= MAX_CPUS)) {
        panic("SPINLOCK: invalid CPU index");
    }

    const uint32_t idx = __atomic_fetch_add(&g_qnode_idx[cpu], 1u, __ATOMIC_RELAXED);

    if (unlikely(idx >= QNODE_DEPTH)) {
        panic("SPINLOCK: queue depth overflow (IRQs nested too deeply)");
    }

    qnode_t* node = &g_qnodes[cpu][idx];

    node->next = NULL;
    node->locked = 1u;

    const uint32_t tail = encode_tail(cpu, idx);

    uint32_t old_val = __atomic_load_n(&lock->val, __ATOMIC_RELAXED);
    uint32_t prev_tail = 0u;

    /*
     * Register ourselves in the lock's tail.
     * This makes us the official end of the queue. We preserve the lowest
     * 16 bits (the locked/pending state) and overwrite the upper 16 bits.
     */
    for (;;) {
        const uint32_t new_val = (old_val & 0x0000FFFFu) | tail;

        const int exchanged = __atomic_compare_exchange_n(
            &lock->val, &old_val, new_val, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
        );

        if (likely(exchanged)) {
            prev_tail = old_val & 0xFFFF0000u;
            break;
        }

        cpu_relax();
    }

    /*
     * If there was someone before us, link our node to theirs
     * and wait for them to unlock us. We spin on our local cache line.
     */
    if (prev_tail != 0u) {
        qnode_t* prev_node = decode_tail(prev_tail);

        __atomic_store_n(&prev_node->next, node, __ATOMIC_RELEASE);

        while (__atomic_load_n(&node->locked, __ATOMIC_ACQUIRE) != 0u) {
            cpu_relax();
        }
    }

    /*
     * We are now at the head of the queue.
     * Wait patiently until the actual lock holder releases the lock byte.
     */
    while (__atomic_load_n(&lock->locked, __ATOMIC_ACQUIRE) != 0u) {
        cpu_relax();
    }

    /*
     * Grab the lock.
     * If we are the ONLY one in the queue (the tail still points to us),
     * we can cleanly remove the tail and set the locked bit in one go.
     */
    uint32_t expected = tail;

    const int grabbed = __atomic_compare_exchange_n(
        &lock->val, &expected, SPINLOCK_LOCKED_VAL,
        0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
    );

    if (likely(grabbed)) {
        goto release_node;
    }

    /*
     * Someone else joined the queue right behind us.
     * We take the lock by writing to the locked byte, leaving their tail intact.
     * We can safely use a non-atomic store here because we are the undisputed
     * head of the queue, and nobody else is allowed to write to the locked byte.
     */
    __atomic_store_n(&lock->locked, 1u, __ATOMIC_RELAXED);

    /* Wait for the next guy to finish linking his node to ours. */
    qnode_t* next;
    
    for (;;) {
        next = (qnode_t*)__atomic_load_n(&node->next, __ATOMIC_ACQUIRE);

        if (likely(next != NULL)) {
            break;
        }

        cpu_relax();
    }

    /* Hand over the queue head status to the next node. */
    __atomic_store_n(&next->locked, 0u, __ATOMIC_RELEASE);

release_node:
    __atomic_fetch_sub(&g_qnode_idx[cpu], 1u, __ATOMIC_RELAXED);
}
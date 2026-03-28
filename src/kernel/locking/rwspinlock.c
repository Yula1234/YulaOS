/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/compiler.h>

#include <hal/cpu.h>

#include "rwspinlock.h"

/*
 * Bit layout for rwspinlock_t::state
 *  - Bit 31: Writer Locked (Exclusive ownership)
 *  - Bit 30: Writer Pending (Blocks new readers, prevents starvation)
 *  - Bits 0-29: Active reader count
 */
#define RW_STATE_WRITER_LOCKED  0x80000000u
#define RW_STATE_WRITER_PENDING 0x40000000u
#define RW_STATE_READER_MASK    0x3FFFFFFFu

void rwspinlock_init(rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    rw->state = 0u;

    spinlock_init(&rw->wait_lock);
}

void rwspinlock_acquire_read(rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    uint32_t s;

    for (;;) {
        s = __atomic_load_n(&rw->state, __ATOMIC_ACQUIRE);

        /*
         * Spin-on-read: wait gently if a writer is active or pending.
         * We do not attempt CAS while these bits are set, avoiding cache bouncing.
         */
        if (unlikely((s & (RW_STATE_WRITER_LOCKED | RW_STATE_WRITER_PENDING)) != 0u)) {
            cpu_relax();
            continue;
        }

        const int acquired = __atomic_compare_exchange_n(
            &rw->state, &s, s + 1u, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
        );

        if (likely(acquired)) {
            return;
        }
    }
}

void rwspinlock_release_read(rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    __atomic_fetch_sub(&rw->state, 1u, __ATOMIC_RELEASE);
}

void rwspinlock_acquire_write(rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    spinlock_acquire(&rw->wait_lock);

    __atomic_fetch_or(&rw->state, RW_STATE_WRITER_PENDING, __ATOMIC_ACQUIRE);

    for (;;) {
        uint32_t s = __atomic_load_n(&rw->state, __ATOMIC_ACQUIRE);

        if (likely((s & RW_STATE_READER_MASK) == 0u)) {
            uint32_t expected = RW_STATE_WRITER_PENDING;

            const int acquired = __atomic_compare_exchange_n(
                &rw->state, &expected, RW_STATE_WRITER_LOCKED,
                0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
            );

            if (likely(acquired)) {
                return;
            }
        }

        cpu_relax();
    }
}

void rwspinlock_release_write(rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    __atomic_fetch_and(&rw->state, ~RW_STATE_WRITER_LOCKED, __ATOMIC_RELEASE);

    /* Hand over the lock to the next queued writer, or allow readers */
    spinlock_release(&rw->wait_lock);
}

uint32_t rwspinlock_acquire_read_safe(rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    rwspinlock_acquire_read(rw);

    return flags;
}

void rwspinlock_release_read_safe(rwspinlock_t* rw, uint32_t flags) {
    rwspinlock_release_read(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

uint32_t rwspinlock_acquire_write_safe(rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    rwspinlock_acquire_write(rw);

    return flags;
}

void rwspinlock_release_write_safe(rwspinlock_t* rw, uint32_t flags) {
    rwspinlock_release_write(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}


void percpu_rwspinlock_init(percpu_rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    rw->writer_active = 0u;
    spinlock_init(&rw->writer_mutex);

    for (int i = 0; i < MAX_CPUS; i++) {
        rw->readers[i].count = 0u;
    }
}

void percpu_rwspinlock_acquire_read(percpu_rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    const int cpu = hal_cpu_index();

    for (;;) {
        /* Wait out the active writer gently */
        if (unlikely(__atomic_load_n(&rw->writer_active, __ATOMIC_ACQUIRE) != 0u)) {
            cpu_relax();
            continue;
        }

        __atomic_fetch_add(&rw->readers[cpu].count, 1u, __ATOMIC_SEQ_CST);

        if (likely(__atomic_load_n(&rw->writer_active, __ATOMIC_SEQ_CST) == 0u)) {
            return;
        }

        /*
         * A writer sneaked in just before we incremented.
         * Back off, restore the count, and wait.
         */
        __atomic_fetch_sub(&rw->readers[cpu].count, 1u, __ATOMIC_SEQ_CST);

        while (__atomic_load_n(&rw->writer_active, __ATOMIC_ACQUIRE) != 0u) {
            cpu_relax();
        }
    }
}

void percpu_rwspinlock_release_read(percpu_rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    const int cpu = hal_cpu_index();

    __atomic_fetch_sub(&rw->readers[cpu].count, 1u, __ATOMIC_RELEASE);
}

void percpu_rwspinlock_acquire_write(percpu_rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    spinlock_acquire(&rw->writer_mutex);

    /*
     * SEQ_CST guarantees visibility of writer_active before we proceed
     * to read the CPU-local reader counts.
     */
    __atomic_store_n(&rw->writer_active, 1u, __ATOMIC_SEQ_CST);

    for (int i = 0; i < MAX_CPUS; i++) {
        while (__atomic_load_n(&rw->readers[i].count, __ATOMIC_ACQUIRE) != 0u) {
            cpu_relax();
        }
    }
}

void percpu_rwspinlock_release_write(percpu_rwspinlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    __atomic_store_n(&rw->writer_active, 0u, __ATOMIC_RELEASE);

    spinlock_release(&rw->writer_mutex);
}

uint32_t percpu_rwspinlock_acquire_read_safe(percpu_rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    percpu_rwspinlock_acquire_read(rw);

    return flags;
}

void percpu_rwspinlock_release_read_safe(percpu_rwspinlock_t* rw, uint32_t flags) {
    percpu_rwspinlock_release_read(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

uint32_t percpu_rwspinlock_acquire_write_safe(percpu_rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    percpu_rwspinlock_acquire_write(rw);

    return flags;
}

void percpu_rwspinlock_release_write_safe(percpu_rwspinlock_t* rw, uint32_t flags) {
    percpu_rwspinlock_release_write(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}
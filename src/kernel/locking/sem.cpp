/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/waitq/waitqueue.h>

#include <lib/compiler.h>

#include <hal/cpu.h>

#include "sem.h"

extern "C" volatile uint32_t timer_ticks;

extern "C" void sem_init(semaphore_t* sem, int init_count) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    sem->count = init_count;

    spinlock_init(&sem->lock);
    waitqueue_init(&sem->waitq, static_cast<void*>(sem), TASK_BLOCK_SEM);
}

extern "C" void sem_reset(semaphore_t* sem, int value) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    if (kernel::unlikely(!dlist_empty(&sem->waitq.waiters))) {
        panic("SEM: reset called with active waiters");
    }

    __atomic_store_n(&sem->count, value, __ATOMIC_RELEASE);

    spinlock_release_safe(&sem->lock, flags);
}

static inline bool sem_try_acquire_fast(semaphore_t* sem) {
    int c = __atomic_load_n(&sem->count, __ATOMIC_RELAXED);

    while (c > 0) {
        const bool acquired = __atomic_compare_exchange_n(
            &sem->count, &c, c - 1, false,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
        );

        if (kernel::likely(acquired)) {
            return true;
        }
    }

    return false;
}

/*
 * Optimistically spin for a short duration before yielding the CPU.
 * This absorbs bursty contention without the cost of context switching.
 */
static inline bool sem_optimistic_spin(semaphore_t* sem) {
    for (uint32_t i = 0; i < 256u; i++) {
        if (kernel::likely(sem_try_acquire_fast(sem))) {
            return true;
        }

        cpu_relax();
    }

    return false;
}

extern "C" int sem_try_acquire(semaphore_t* sem) {
    if (kernel::unlikely(!sem)) {
        return 0;
    }

    if (kernel::likely(sem_try_acquire_fast(sem))) {
        return 1;
    }

    return 0;
}

extern "C" void sem_wait(semaphore_t* sem) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    task_t* curr = proc_current();

    if (kernel::unlikely(!curr)) {
        /*
         * Early boot or contextless execution.
         * We cannot sleep, so we must spin indefinitely.
         */
        while (!sem_try_acquire_fast(sem)) {
            cpu_relax();
        }

        return;
    }

    if (kernel::likely(sem_try_acquire_fast(sem))) {
        curr->blocked_on_sem = nullptr;
        curr->blocked_kind = TASK_BLOCK_NONE;
        return;
    }

    if (kernel::likely(sem_optimistic_spin(sem))) {
        curr->blocked_on_sem = nullptr;
        curr->blocked_kind = TASK_BLOCK_NONE;
        return;
    }

    for (;;) {
        uint32_t flags = spinlock_acquire_safe(&sem->lock);

        if (sem->count > 0) {
            __atomic_fetch_sub(&sem->count, 1, __ATOMIC_ACQUIRE);
            spinlock_release_safe(&sem->lock, flags);

            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
            return;
        }

        (void)waitqueue_wait_prepare_locked(&sem->waitq, curr);

        spinlock_release_safe(&sem->lock, flags);

        sched_yield();

        sem_remove_task(curr);

        /*
         * Upon waking, attempt to aggressively acquire the token via the
         * fast path to allow "barging", which increases overall throughput.
         */
        if (kernel::likely(sem_try_acquire_fast(sem))) {

            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
            return;
        }
    }
}

extern "C" int sem_wait_timeout(semaphore_t* sem, uint32_t deadline_tick) {
    if (kernel::unlikely(!sem)) {
        return 0;
    }

    task_t* curr = proc_current();

    if (kernel::unlikely(!curr)) {
        while (!sem_try_acquire_fast(sem)) {
            if (kernel::unlikely((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u)) {
                return 0;
            }

            cpu_relax();
        }

        return 1;
    }

    if (kernel::likely(sem_try_acquire_fast(sem))) {
        curr->blocked_on_sem = nullptr;
        curr->blocked_kind = TASK_BLOCK_NONE;
        return 1;
    }

    if (kernel::likely(sem_optimistic_spin(sem))) {
        curr->blocked_on_sem = nullptr;
        curr->blocked_kind = TASK_BLOCK_NONE;
        return 1;
    }

    for (;;) {
        if (kernel::unlikely((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u)) {
            return 0;
        }

        uint32_t flags = spinlock_acquire_safe(&sem->lock);

        if (sem->count > 0) {
            __atomic_fetch_sub(&sem->count, 1, __ATOMIC_ACQUIRE);
            spinlock_release_safe(&sem->lock, flags);

            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
            return 1;
        }

        (void)waitqueue_wait_prepare_locked(&sem->waitq, curr);

        spinlock_release_safe(&sem->lock, flags);

        if (curr->blocked_on_sem == sem) {
            proc_sleep_add(curr, deadline_tick);
        }

        sem_remove_task(curr);

        if (kernel::likely(sem_try_acquire_fast(sem))) {
            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
            return 1;
        }

        if (kernel::unlikely((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u)) {
            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
            return 0;
        }
    }
}

extern "C" void sem_signal(semaphore_t* sem) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    task_t* to_wake = nullptr;

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);

    to_wake = waitqueue_dequeue_locked(&sem->waitq);

    spinlock_release_safe(&sem->lock, flags);

    if (to_wake) {
        waitqueue_wake_task(to_wake);
    }
}

extern "C" void sem_signal_all(semaphore_t* sem) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    dlist_head_t local;
    waitqueue_detach_all_locked(&sem->waitq, &local);

    uint32_t detached_count = 0u;

    for (dlist_head_t* it = local.next; it != &local; it = it->next) {
        detached_count++;
    }

    if (detached_count != 0u) {
        __atomic_fetch_add(&sem->count, (int)detached_count, __ATOMIC_RELEASE);
    }

    spinlock_release_safe(&sem->lock, flags);

    waitqueue_wake_detached_list(&local);
}

extern "C" void sem_remove_task(task_t* t) {
    if (kernel::unlikely(!t)) {
        return;
    }

    void* raw_sem = __atomic_load_n(&t->blocked_on_sem, __ATOMIC_ACQUIRE);
    if (!raw_sem) {
        return;
    }

    if (kernel::unlikely(t->blocked_kind != TASK_BLOCK_SEM)) {
        return;
    }

    semaphore_t* sem = static_cast<semaphore_t*>(raw_sem);

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    waitqueue_wait_cancel_locked(&sem->waitq, t);

    spinlock_release_safe(&sem->lock, flags);
}
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

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
    dlist_init(&sem->wait_list);
}

extern "C" void sem_reset(semaphore_t* sem, int value) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    if (kernel::unlikely(!dlist_empty(&sem->wait_list))) {
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

        curr->blocked_on_sem = static_cast<void*>(sem);
        curr->blocked_kind = TASK_BLOCK_SEM;

        dlist_add_tail(&curr->sem_node, &sem->wait_list);

        if (kernel::unlikely(proc_change_state(curr, TASK_WAITING) != 0)) {
            dlist_del(&curr->sem_node);

            curr->sem_node.next = nullptr;
            curr->sem_node.prev = nullptr;
            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
        }

        spinlock_release_safe(&sem->lock, flags);

        sched_yield();

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

        curr->blocked_on_sem = static_cast<void*>(sem);
        curr->blocked_kind = TASK_BLOCK_SEM;

        dlist_add_tail(&curr->sem_node, &sem->wait_list);

        if (kernel::unlikely(proc_change_state(curr, TASK_WAITING) != 0)) {
            dlist_del(&curr->sem_node);

            curr->sem_node.next = nullptr;
            curr->sem_node.prev = nullptr;
            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
        }

        spinlock_release_safe(&sem->lock, flags);

        if (curr->blocked_on_sem == sem) {
            proc_sleep_add(curr, deadline_tick);
        }

        if (kernel::likely(sem_try_acquire_fast(sem))) {
            curr->blocked_on_sem = nullptr;
            curr->blocked_kind = TASK_BLOCK_NONE;
            return 1;
        }

        if (kernel::unlikely((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u)) {
            sem_remove_task(curr);

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

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);

    while (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        __atomic_fetch_add(&t->in_transit, 1u, __ATOMIC_ACQUIRE);

        dlist_del(&t->sem_node);

        t->sem_node.next = nullptr;
        t->sem_node.prev = nullptr;
        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;

        if (kernel::unlikely(t->state == TASK_ZOMBIE || t->state == TASK_UNUSED)) {
            __atomic_fetch_sub(&t->in_transit, 1u, __ATOMIC_RELEASE);
            continue;
        }

        if (kernel::likely(proc_change_state(t, TASK_RUNNABLE) == 0)) {
            sched_add(t);
        }

        __atomic_fetch_sub(&t->in_transit, 1u, __ATOMIC_RELEASE);

        /*
         * Wake only the first valid task. The task itself will consume the token
         * when it wakes up and attempts to acquire the semaphore.
         */
        break;
    }

    spinlock_release_safe(&sem->lock, flags);
}

extern "C" void sem_signal_all(semaphore_t* sem) {
    if (kernel::unlikely(!sem)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    if (kernel::unlikely(dlist_empty(&sem->wait_list))) {
        __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);
        spinlock_release_safe(&sem->lock, flags);
        return;
    }

    while (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        __atomic_fetch_add(&t->in_transit, 1u, __ATOMIC_ACQUIRE);

        dlist_del(&t->sem_node);

        t->sem_node.next = nullptr;
        t->sem_node.prev = nullptr;
        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;

        __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);

        if (kernel::likely(t->state != TASK_ZOMBIE && t->state != TASK_UNUSED)) {
            if (kernel::likely(proc_change_state(t, TASK_RUNNABLE) == 0)) {
                sched_add(t);
            }
        }

        __atomic_fetch_sub(&t->in_transit, 1u, __ATOMIC_RELEASE);
    }

    spinlock_release_safe(&sem->lock, flags);
}

extern "C" void sem_remove_task(task_t* t) {
    if (kernel::unlikely(!t || !t->blocked_on_sem)) {
        return;
    }

    if (kernel::unlikely(t->blocked_kind != TASK_BLOCK_SEM)) {
        return;
    }

    semaphore_t* sem = static_cast<semaphore_t*>(t->blocked_on_sem);

    const uint32_t flags = spinlock_acquire_safe(&sem->lock);

    if (kernel::likely(t->blocked_on_sem == sem && t->blocked_kind == TASK_BLOCK_SEM)) {
        if (t->sem_node.next && t->sem_node.prev) {
            dlist_del(&t->sem_node);
            t->sem_node.next = nullptr;
            t->sem_node.prev = nullptr;
        }

        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;
    }

    spinlock_release_safe(&sem->lock, flags);
}
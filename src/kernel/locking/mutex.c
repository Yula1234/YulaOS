/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <lib/compiler.h>

#include <hal/cpu.h>

#include "mutex.h"

#define MUTEX_FLAG_WAITERS 1UL

void mutex_init(mutex_t* m) {
    if (unlikely(!m)) {
        return;
    }

    m->owner = 0u;

    spinlock_init(&m->wait_lock);
    dlist_init(&m->wait_list);
}

/*
 * Adaptive optimistic spinning.
 * Returns 1 if the lock became free and we should attempt to grab it.
 * Returns 0 if we should stop spinning and yield the CPU.
 */
static int mutex_spin_on_owner(mutex_t* m) {
    for (uint32_t i = 0; i < 1024u; i++) {
        const uintptr_t owner_val = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);

        if (likely(owner_val == 0u)) {
            return 1;
        }

        task_t* owner_task = (task_t*)(owner_val & ~MUTEX_FLAG_WAITERS);

        if (likely(owner_task)) {
            /*
             * If the owner is not actively running (e.g., blocked on I/O),
             * spinning is a pure waste of CPU cycles. Abort immediately.
             */
            const task_state_t owner_state = 
                (task_state_t)__atomic_load_n(&owner_task->state, __ATOMIC_RELAXED);

            if (unlikely(owner_state != TASK_RUNNING)) {
                return 0;
            }
        }

        cpu_relax();
    }

    return 0;
}

static void mutex_lock_slowpath(mutex_t* m, task_t* curr) {
    const uintptr_t curr_val = (uintptr_t)curr;

    if (mutex_spin_on_owner(m)) {
        uintptr_t expected = 0u;

        const int acquired = __atomic_compare_exchange_n(
            &m->owner, &expected, curr_val, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
        );

        if (likely(acquired)) {
            return;
        }
    }

    uint32_t flags = spinlock_acquire_safe(&m->wait_lock);

    for (;;) {
        uintptr_t owner = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);

        if (owner == 0u) {
            uintptr_t new_owner = curr_val;

            if (unlikely(!dlist_empty(&m->wait_list))) {
                new_owner |= MUTEX_FLAG_WAITERS;
            }

            const int acquired = __atomic_compare_exchange_n(
                &m->owner, &owner, new_owner, 0,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
            );

            if (likely(acquired)) {
                break;
            }

            continue;
        }

        if (likely((owner & MUTEX_FLAG_WAITERS) == 0u)) {
            const uintptr_t with_waiters = owner | MUTEX_FLAG_WAITERS;

            const int updated = __atomic_compare_exchange_n(
                &m->owner, &owner, with_waiters, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
            );

            if (unlikely(!updated)) {
                continue;
            }
        }

        curr->blocked_on_sem = (void*)m;
        curr->blocked_kind = TASK_BLOCK_SEM;

        dlist_add_tail(&curr->sem_node, &m->wait_list);

        if (unlikely(proc_change_state(curr, TASK_WAITING) != 0)) {
            dlist_del(&curr->sem_node);

            curr->sem_node.next = 0;
            curr->sem_node.prev = 0;
            curr->blocked_on_sem = 0;
            curr->blocked_kind = TASK_BLOCK_NONE;
        }

        spinlock_release_safe(&m->wait_lock, flags);

        sched_yield();

        flags = spinlock_acquire_safe(&m->wait_lock);
    }

    spinlock_release_safe(&m->wait_lock, flags);
}

void mutex_lock(mutex_t* m) {
    if (unlikely(!m)) {
        return;
    }

    task_t* curr = proc_current();

    const uintptr_t curr_val = (uintptr_t)curr;

    uintptr_t expected = 0u;

    const int acquired = __atomic_compare_exchange_n(
        &m->owner, &expected, curr_val, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
    );

    if (likely(acquired)) {
        return;
    }

    mutex_lock_slowpath(m, curr);
}

static void mutex_unlock_slowpath(mutex_t* m, task_t* curr) {
    const uintptr_t curr_val = (uintptr_t)curr;
    const uintptr_t current_owner = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);

    if (unlikely((current_owner & ~MUTEX_FLAG_WAITERS) != curr_val)) {
        panic("MUTEX: unlock by non-owner task");
    }

    uint32_t flags = spinlock_acquire_safe(&m->wait_lock);

    /*
     * Drop the lock entirely. The next woken task will take it and 
     * re-establish the MUTEX_FLAG_WAITERS bit if the queue remains non-empty.
     */
    __atomic_store_n(&m->owner, 0u, __ATOMIC_RELEASE);

    if (likely(!dlist_empty(&m->wait_list))) {
        task_t* t = container_of(m->wait_list.next, task_t, sem_node);

        __sync_fetch_and_add(&t->in_transit, 1u);

        dlist_del(&t->sem_node);

        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;
        t->blocked_kind = TASK_BLOCK_NONE;

        if (likely(t->state != TASK_ZOMBIE && t->state != TASK_UNUSED)) {
            if (likely(proc_change_state(t, TASK_RUNNABLE) == 0)) {
                sched_add(t);
            }
        }

        __sync_fetch_and_sub(&t->in_transit, 1u);
    }

    spinlock_release_safe(&m->wait_lock, flags);
}

void mutex_unlock(mutex_t* m) {
    if (unlikely(!m)) {
        return;
    }

    task_t* curr = proc_current();
    const uintptr_t curr_val = (uintptr_t)curr;

    uintptr_t expected = curr_val;

    /*
     * Fast-path unlock. 
     * This will intentionally fail if MUTEX_FLAG_WAITERS is set, correctly
     * forcing the unlocker into the slowpath to wake up waiting tasks.
     */
    const int released = __atomic_compare_exchange_n(
        &m->owner, &expected, 0u, 0, __ATOMIC_RELEASE,
        __ATOMIC_RELAXED
    );

    if (likely(released)) {
        return;
    }

    mutex_unlock_slowpath(m, curr);
}

int mutex_try_lock(mutex_t* m) {
    if (unlikely(!m)) {
        return 0;
    }

    task_t* curr = proc_current();
    const uintptr_t curr_val = (uintptr_t)curr;

    uintptr_t expected = 0u;

    const int acquired = __atomic_compare_exchange_n(
        &m->owner, &expected, curr_val, 0,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
    );

    return acquired ? 1 : 0;
}
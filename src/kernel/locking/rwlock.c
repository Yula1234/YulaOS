/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <lib/compiler.h>

#include <hal/cpu.h>

#include "rwlock.h"

#define RWLOCK_WRITER         0x80000000u
#define RWLOCK_WAITING_WRITER 0x40000000u
#define RWLOCK_WAITING_READER 0x20000000u
#define RWLOCK_READER_MASK    0x1FFFFFFFu

void rwlock_init(rwlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    rw->state_ = 0u;
    rw->writer_owner_ = 0u;

    spinlock_init(&rw->wait_lock_);

    dlist_init(&rw->read_waiters_);
    dlist_init(&rw->write_waiters_);
}

/*
 * Wake up pending tasks.
 *
 * This function enforces Writer Preference. If there are pending writers,
 * we wake exactly one writer. Only if there are no pending writers do we
 * wake all pending readers simultaneously.
 */
static void rwlock_wake_waiters(rwlock_t* rw) {
    const uint32_t flags = spinlock_acquire_safe(&rw->wait_lock_);

    if (likely(!dlist_empty(&rw->write_waiters_))) {
        task_t* t = container_of(rw->write_waiters_.next, task_t, sem_node);

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

        if (unlikely(dlist_empty(&rw->write_waiters_))) {
            __atomic_fetch_and(&rw->state_, ~RWLOCK_WAITING_WRITER, __ATOMIC_RELAXED);
        }
    } else if (unlikely(!dlist_empty(&rw->read_waiters_))) {
        while (!dlist_empty(&rw->read_waiters_)) {
            task_t* t = container_of(rw->read_waiters_.next, task_t, sem_node);

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

        __atomic_fetch_and(&rw->state_, ~RWLOCK_WAITING_READER, __ATOMIC_RELAXED);
    }

    spinlock_release_safe(&rw->wait_lock_, flags);
}

/*
 * Optimistically spin for a short duration while a writer holds the lock.
 * This avoids costly context switches if the writer completes its critical
 * section quickly on another CPU.
 */
static int rwlock_spin_on_writer(rwlock_t* rw) {
    for (uint32_t i = 0; i < 1024u; i++) {
        const uint32_t s = __atomic_load_n(&rw->state_, __ATOMIC_ACQUIRE);

        if (likely((s & RWLOCK_WRITER) == 0u)) {
            return 1;
        }

        const uintptr_t owner_val = __atomic_load_n(&rw->writer_owner_, __ATOMIC_RELAXED);

        if (likely(owner_val != 0u)) {
            task_t* owner_task = (task_t*)owner_val;

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

static void rwlock_acquire_read_slowpath(rwlock_t* rw) {
    task_t* curr = proc_current();

    for (;;) {
        if (rwlock_spin_on_writer(rw)) {
            uint32_t s = __atomic_load_n(&rw->state_, __ATOMIC_ACQUIRE);

            if (likely((s & (RWLOCK_WRITER | RWLOCK_WAITING_WRITER)) == 0u)) {
                const int acquired = __atomic_compare_exchange_n(
                    &rw->state_, &s, s + 1u, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
                );

                if (likely(acquired)) {
                    return;
                }

                continue;
            }
        }

        uint32_t flags = spinlock_acquire_safe(&rw->wait_lock_);

        uint32_t s = __atomic_load_n(&rw->state_, __ATOMIC_ACQUIRE);

        if (likely((s & (RWLOCK_WRITER | RWLOCK_WAITING_WRITER)) == 0u)) {
            spinlock_release_safe(&rw->wait_lock_, flags);
            continue;
        }

        if (likely((s & RWLOCK_WAITING_READER) == 0u)) {
            __atomic_fetch_or(&rw->state_, RWLOCK_WAITING_READER, __ATOMIC_RELAXED);
        }

        curr->blocked_on_sem = (void*)rw;
        curr->blocked_kind = TASK_BLOCK_SEM;

        dlist_add_tail(&curr->sem_node, &rw->read_waiters_);

        if (unlikely(proc_change_state(curr, TASK_WAITING) != 0)) {
            dlist_del(&curr->sem_node);

            curr->sem_node.next = 0;
            curr->sem_node.prev = 0;
            curr->blocked_on_sem = 0;
            curr->blocked_kind = TASK_BLOCK_NONE;
        }

        spinlock_release_safe(&rw->wait_lock_, flags);

        sched_yield();
    }
}

void rwlock_acquire_read(rwlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    uint32_t s = __atomic_load_n(&rw->state_, __ATOMIC_ACQUIRE);

    while (likely((s & (RWLOCK_WRITER | RWLOCK_WAITING_WRITER)) == 0u)) {
        const int acquired = __atomic_compare_exchange_n(
            &rw->state_, &s, s + 1u, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
        );

        if (likely(acquired)) {
            return;
        }
    }

    rwlock_acquire_read_slowpath(rw);
}

void rwlock_release_read(rwlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    const uint32_t s = __atomic_fetch_sub(&rw->state_, 1u, __ATOMIC_RELEASE);

    /*
     * If we are the last active reader, and there is a writer waiting,
     * we must wake it up to hand over the lock.
     */
    if (unlikely((s & RWLOCK_READER_MASK) == 1u)) {
        if (unlikely((s & RWLOCK_WAITING_WRITER) != 0u)) {
            rwlock_wake_waiters(rw);
        }
    }
}

static void rwlock_acquire_write_slowpath(rwlock_t* rw, task_t* curr) {
    const uintptr_t curr_val = (uintptr_t)curr;

    for (;;) {
        if (rwlock_spin_on_writer(rw)) {
            uint32_t s = __atomic_load_n(&rw->state_, __ATOMIC_ACQUIRE);

            if (likely((s & RWLOCK_WRITER) == 0u && (s & RWLOCK_READER_MASK) == 0u)) {
                const int acquired = __atomic_compare_exchange_n(
                    &rw->state_, &s, s | RWLOCK_WRITER, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
                );

                if (likely(acquired)) {
                    __atomic_store_n(&rw->writer_owner_, curr_val, __ATOMIC_RELAXED);
                    return;
                }

                continue;
            }
        }

        uint32_t flags = spinlock_acquire_safe(&rw->wait_lock_);

        uint32_t s = __atomic_load_n(&rw->state_, __ATOMIC_ACQUIRE);

        if (likely((s & RWLOCK_WRITER) == 0u && (s & RWLOCK_READER_MASK) == 0u)) {
            spinlock_release_safe(&rw->wait_lock_, flags);
            continue;
        }

        if (likely((s & RWLOCK_WAITING_WRITER) == 0u)) {
            __atomic_fetch_or(&rw->state_, RWLOCK_WAITING_WRITER, __ATOMIC_RELAXED);
        }

        curr->blocked_on_sem = (void*)rw;
        curr->blocked_kind = TASK_BLOCK_SEM;

        dlist_add_tail(&curr->sem_node, &rw->write_waiters_);

        if (unlikely(proc_change_state(curr, TASK_WAITING) != 0)) {
            dlist_del(&curr->sem_node);

            curr->sem_node.next = 0;
            curr->sem_node.prev = 0;
            curr->blocked_on_sem = 0;
            curr->blocked_kind = TASK_BLOCK_NONE;
        }

        spinlock_release_safe(&rw->wait_lock_, flags);

        sched_yield();
    }
}

void rwlock_acquire_write(rwlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    task_t* curr = proc_current();
    const uintptr_t curr_val = (uintptr_t)curr;

    uint32_t expected = 0u;

    const int acquired = __atomic_compare_exchange_n(
        &rw->state_, &expected, RWLOCK_WRITER,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED
    );

    if (likely(acquired)) {
        __atomic_store_n(&rw->writer_owner_, curr_val, __ATOMIC_RELAXED);
        return;
    }

    rwlock_acquire_write_slowpath(rw, curr);
}

void rwlock_release_write(rwlock_t* rw) {
    if (unlikely(!rw)) {
        return;
    }

    __atomic_store_n(&rw->writer_owner_, 0u, __ATOMIC_RELAXED);

    const uint32_t s = __atomic_fetch_and(&rw->state_, ~RWLOCK_WRITER, __ATOMIC_RELEASE);

    /*
     * If there are waiting writers or waiting readers, we must invoke
     * the wakeup mechanism to handle handoffs cleanly.
     */
    if (unlikely((s & (RWLOCK_WAITING_WRITER | RWLOCK_WAITING_READER)) != 0u)) {
        rwlock_wake_waiters(rw);
    }
}
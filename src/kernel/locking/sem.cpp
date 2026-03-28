/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <kernel/locking/sem.h>
#include <kernel/panic.h>
#include <lib/cpp/lock_guard.h>

#include <kernel/proc.h>
#include <kernel/sched.h>

extern "C" volatile uint32_t timer_ticks;

extern "C" void sem_init(semaphore_t* sem, int init_count) {
    sem->count = init_count;
    spinlock_init(&sem->lock);
    dlist_init(&sem->wait_list);
}

extern "C" void sem_reset(semaphore_t* sem, int value) {
    if (!sem) {
        return;
    }

    kernel::SpinLockNativeSafeGuard guard(sem->lock);

    if (!dlist_empty(&sem->wait_list)) {
        panic("SEM: reset with waiters");
    }

    __atomic_store_n(&sem->count, value, __ATOMIC_RELEASE);
}

static inline bool sem_try_acquire_fast(semaphore_t* sem) {
    int c = __atomic_load_n(&sem->count, __ATOMIC_RELAXED);
    while (c > 0) {
        if (__atomic_compare_exchange_n(
                &sem->count, &c, c - 1,
                false, __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED
            )) {
            return true;
        }
    }

    return false;
}

extern "C" int sem_try_acquire(semaphore_t* sem) {
    if (sem_try_acquire_fast(sem)) {
        return 1;
    }

    kernel::SpinLockNativeSafeGuard guard(sem->lock);

    if (sem->count <= 0) {
        return 0;
    }

    __atomic_fetch_sub(&sem->count, 1, __ATOMIC_ACQUIRE);
    return 1;
}

extern "C" void sem_wait(semaphore_t* sem) {
    while (1) {
        if (sem_try_acquire_fast(sem)) {
            task_t* curr = proc_current();
            if (curr) {
                curr->blocked_on_sem = nullptr;
                curr->blocked_kind = TASK_BLOCK_NONE;
            }
            return;
        }

        task_t* curr_fast = proc_current();
        if (!curr_fast) {
            while (!sem_try_acquire_fast(sem)) {
                __asm__ volatile("pause" ::: "memory");
            }
            return;
        }

        {
            kernel::SpinLockNativeSafeGuard guard(sem->lock);

            if (sem->count > 0) {
                __atomic_fetch_sub(&sem->count, 1, __ATOMIC_ACQUIRE);
                task_t* curr = proc_current();
                if (curr) {
                    curr->blocked_on_sem = nullptr;
                    curr->blocked_kind = TASK_BLOCK_NONE;
                }
                return;
            }

            task_t* curr = proc_current();
            if (!curr) {
                continue;
            }

            curr->blocked_on_sem = static_cast<void*>(sem);
            curr->blocked_kind = TASK_BLOCK_SEM;

            dlist_add_tail(&curr->sem_node, &sem->wait_list);

            if (proc_change_state(curr, TASK_WAITING) != 0) {
                dlist_del(&curr->sem_node);

                curr->sem_node.next = nullptr;
                curr->sem_node.prev = nullptr;
                curr->blocked_on_sem = nullptr;
                curr->blocked_kind = TASK_BLOCK_NONE;
            }
        }

        sched_yield();
    }
}

extern "C" int sem_wait_timeout(semaphore_t* sem, uint32_t deadline_tick) {
    if (!sem) {
        return 0;
    }

    const auto unblock_curr = [](task_t* curr) {
        if (!curr) {
            return;
        }

        proc_sleep_remove(curr);
        curr->blocked_on_sem = nullptr;
        curr->blocked_kind = TASK_BLOCK_NONE;
    };

    while (1) {
        task_t* curr = proc_current();

        if (sem_try_acquire_fast(sem)) {
            unblock_curr(curr);
            return 1;
        }

        if ((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u) {
            unblock_curr(curr);
            return 0;
        }

        if (!curr) {
            while (!sem_try_acquire_fast(sem)) {
                if ((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u) {
                    return 0;
                }
                __asm__ volatile("pause" ::: "memory");
            }
            return 1;
        }

        {
            kernel::SpinLockNativeSafeGuard guard(sem->lock);

            if (sem->count > 0) {
                __atomic_fetch_sub(&sem->count, 1, __ATOMIC_ACQUIRE);
                unblock_curr(curr);
                return 1;
            }

            curr->blocked_on_sem = static_cast<void*>(sem);
            curr->blocked_kind = TASK_BLOCK_SEM;
            dlist_add_tail(&curr->sem_node, &sem->wait_list);

            if (proc_change_state(curr, TASK_WAITING) != 0) {
                dlist_del(&curr->sem_node);

                curr->sem_node.next = nullptr;
                curr->sem_node.prev = nullptr;
                curr->blocked_on_sem = nullptr;
                curr->blocked_kind = TASK_BLOCK_NONE;
            }
        }

        if (curr->blocked_on_sem != sem) {
            continue;
        }

        proc_sleep_add(curr, deadline_tick);

        if (curr->blocked_on_sem != sem) {
            continue;
        }

        if ((uint32_t)(timer_ticks - deadline_tick) < 0x80000000u) {
            sem_remove_task(curr);
            unblock_curr(curr);
            return 0;
        }
    }
}

extern "C" void sem_signal(semaphore_t* sem) {
    kernel::SpinLockNativeSafeGuard guard(sem->lock);

    __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);

    while (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        __sync_fetch_and_add(&t->in_transit, 1);

        dlist_del(&t->sem_node);

        t->sem_node.next = nullptr;
        t->sem_node.prev = nullptr;
        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;

        if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
            __sync_fetch_and_sub(&t->in_transit, 1);
            continue;
        }

        if (proc_change_state(t, TASK_RUNNABLE) == 0) {
            sched_add(t);
        }

        __sync_fetch_and_sub(&t->in_transit, 1);

        break;
    }
}

extern "C" void sem_signal_all(semaphore_t* sem) {
    kernel::SpinLockNativeSafeGuard guard(sem->lock);

    if (dlist_empty(&sem->wait_list)) {
        __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);
        return;
    }

    while (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        __sync_fetch_and_add(&t->in_transit, 1);

        dlist_del(&t->sem_node);

        t->sem_node.next = nullptr;
        t->sem_node.prev = nullptr;
        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;

        __atomic_fetch_add(&sem->count, 1, __ATOMIC_RELEASE);

        if (proc_change_state(t, TASK_RUNNABLE) == 0) {
            sched_add(t);
        }

        __sync_fetch_and_sub(&t->in_transit, 1);
    }
}

extern "C" void sem_remove_task(task_t* t) {
    if (!t || !t->blocked_on_sem) {
        return;
    }

    if (t->blocked_kind != TASK_BLOCK_SEM) {
        return;
    }

    semaphore_t* sem = static_cast<semaphore_t*>(t->blocked_on_sem);

    {
        kernel::SpinLockNativeSafeGuard guard(sem->lock);

        if (t->blocked_on_sem != sem) {
            return;
        }

        if (t->sem_node.next && t->sem_node.prev) {
            dlist_del(&t->sem_node);
            t->sem_node.next = nullptr;
            t->sem_node.prev = nullptr;
        }

        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;
    }
}

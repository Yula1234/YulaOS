// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "poll_waitq.h"
#include <kernel/proc.h>
#include <kernel/panic.h>
#include <hal/lock.h>

extern "C" {

static int poll_waitq_waiter_is_clean(const poll_waiter_t* w) {
    if (!w) {
        return 0;
    }

    if (w->task != nullptr || w->q != nullptr) {
        return 0;
    }

    if (w->q_node.next != nullptr || w->q_node.prev != nullptr) {
        return 0;
    }

    if (w->task_node.next != nullptr || w->task_node.prev != nullptr) {
        return 0;
    }

    return 1;
}

static int poll_waitq_try_unlink_node(dlist_head_t* node) {
    if (!node) {
        return 0;
    }

    return dlist_unlink_consistent(node);
}

void poll_waitq_init(poll_waitq_t* q) {
    if (!q) return;
    spinlock_init(&q->lock);
    dlist_init(&q->waiters);

    q->refs = 1u;
    q->finalize = nullptr;
    q->finalize_ctx = nullptr;
}

void poll_waitq_init_finalizable(
    poll_waitq_t* q,
    void (*finalize)(void* ctx),
    void* finalize_ctx
) {
    if (!q) {
        return;
    }

    poll_waitq_init(q);

    q->finalize = finalize;
    q->finalize_ctx = finalize_ctx;
}

void poll_waitq_retain(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    __atomic_fetch_add(&q->refs, 1u, __ATOMIC_RELAXED);
}

void poll_waitq_put(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    uint32_t old_refs = __atomic_fetch_sub(&q->refs, 1u, __ATOMIC_ACQ_REL);

    if (old_refs == 0u) {
        panic("POLL: waitq_put underflow");
    }

    if (old_refs == 1u) {
        void (*finalize)(void*) = q->finalize;
        void* finalize_ctx = q->finalize_ctx;

        if (!finalize) {
            panic("POLL: waitq finalized without finalizer");
        }

        finalize(finalize_ctx);
    }
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task) {
    if (!q || !w || !task) return -1;

    uint32_t q_flags = spinlock_acquire_safe(&q->lock);

    if (!poll_waitq_waiter_is_clean(w)) {
        spinlock_release_safe(&q->lock, q_flags);
        return -1;
    }

    if (!proc_task_retain(task)) {
        spinlock_release_safe(&q->lock, q_flags);
        return -1;
    }

    poll_waitq_retain(q);

    spinlock_acquire(&task->poll_lock);

    w->task = task;
    w->q = q;

    dlist_add_tail(&w->q_node, &q->waiters);
    dlist_add_tail(&w->task_node, &task->poll_waiters);

    spinlock_release(&task->poll_lock);
    spinlock_release_safe(&q->lock, q_flags);

    return 0;
}

void poll_waitq_unregister(poll_waiter_t* w) {
    if (!w) return;

    task_t* task = w->task;
    poll_waitq_t* q = w->q;

    if (!q && !task) {
        return;
    }

    if (q) {
        uint32_t q_flags = spinlock_acquire_safe(&q->lock);

        if (task) {
            spinlock_acquire(&task->poll_lock);
        }

        if (poll_waitq_try_unlink_node(&w->q_node)) {
            w->q = nullptr;
        }

        if (task) {
            if (poll_waitq_try_unlink_node(&w->task_node)) {
                w->task = nullptr;
                proc_task_put(task);
            }
            spinlock_release(&task->poll_lock);
        }

        poll_waitq_put(q);

        spinlock_release_safe(&q->lock, q_flags);
    } else if (task) {
        spinlock_acquire(&task->poll_lock);

        if (poll_waitq_try_unlink_node(&w->task_node)) {
            w->task = nullptr;
            proc_task_put(task);
        }

        spinlock_release(&task->poll_lock);
    }
}

void poll_waitq_wake_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t q_flags = spinlock_acquire_safe(&q->lock);

    for (dlist_head_t* it = q->waiters.next; it != &q->waiters; it = it->next) {
        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);
        task_t* task = w->task;
        if (task) {
            proc_wake(task);
        }
    }

    spinlock_release_safe(&q->lock, q_flags);
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t q_flags = spinlock_acquire_safe(&q->lock);

    while (!dlist_empty(&q->waiters)) {
        poll_waiter_t* w = container_of(q->waiters.next, poll_waiter_t, q_node);
        task_t* task = w->task;

        if (task) {
            spinlock_acquire(&task->poll_lock);

            if (poll_waitq_try_unlink_node(&w->task_node)) {
                w->task = nullptr;
                proc_task_put(task);
            }

            spinlock_release(&task->poll_lock);
        }

        if (poll_waitq_try_unlink_node(&w->q_node)) {
            w->q = nullptr;
        }

        poll_waitq_put(q);
    }

    spinlock_release_safe(&q->lock, q_flags);
}

void poll_task_cleanup(struct task* task) {
    if (!task) return;

restart:
    spinlock_acquire(&task->poll_lock);

    while (!dlist_empty(&task->poll_waiters)) {
        poll_waiter_t* w = container_of(task->poll_waiters.next, poll_waiter_t, task_node);
        poll_waitq_t* q = w->q;

        if (q) {
            if (!spinlock_try_acquire(&q->lock)) {
                spinlock_release(&task->poll_lock);
                goto restart;
            }

            if (poll_waitq_try_unlink_node(&w->q_node)) {
                w->q = nullptr;
            }

            if (poll_waitq_try_unlink_node(&w->task_node)) {
                if (w->task) {
                    w->task = nullptr;
                    proc_task_put(task);
                }
            }

            poll_waitq_put(q);
            spinlock_release(&q->lock);
        } else {
            if (poll_waitq_try_unlink_node(&w->task_node)) {
                if (w->task) {
                    w->task = nullptr;
                    proc_task_put(task);
                }
            }
        }
    }

    spinlock_release(&task->poll_lock);
}

}

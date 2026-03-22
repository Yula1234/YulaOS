// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "poll_waitq.h"
#include <kernel/proc.h>
#include <kernel/panic.h>
#include <hal/lock.h>

extern "C" {

static spinlock_t g_poll_global_lock = {0};

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

    for (;;) {
        uint32_t expected = __atomic_load_n(&q->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            panic("POLL: waitq_retain after free");
        }

        if (__atomic_compare_exchange_n(
                &q->refs,
                &expected,
                expected + 1u,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED
            )) {
            return;
        }
    }
}

void poll_waitq_put(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    void (*finalize)(void*) = nullptr;
    void* finalize_ctx = nullptr;

    for (;;) {
        uint32_t expected = __atomic_load_n(&q->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            panic("POLL: waitq_put underflow");
        }

        const uint32_t desired = expected - 1u;
        if (__atomic_compare_exchange_n(
                &q->refs,
                &expected,
                desired,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED
            )) {
            if (desired != 0u) {
                return;
            }

            finalize = q->finalize;
            finalize_ctx = q->finalize_ctx;
            break;
        }
    }

    if (!finalize) {
        panic("POLL: waitq finalized without finalizer");
    }

    finalize(finalize_ctx);
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task) {
    if (!q || !w || !task) return -1;

    uint32_t flags = spinlock_acquire_safe(&g_poll_global_lock);

    if (!poll_waitq_waiter_is_clean(w)) {
        spinlock_release_safe(&g_poll_global_lock, flags);
        return -1;
    }

    if (!proc_task_retain(task)) {
        spinlock_release_safe(&g_poll_global_lock, flags);
        return -1;
    }

    poll_waitq_retain(q);

    w->task = task;
    w->q = q;

    dlist_add_tail(&w->q_node, &q->waiters);
    dlist_add_tail(&w->task_node, &task->poll_waiters);

    spinlock_release_safe(&g_poll_global_lock, flags);

    return 0;
}

void poll_waitq_unregister(poll_waiter_t* w) {
    if (!w) return;

    uint32_t flags = spinlock_acquire_safe(&g_poll_global_lock);

    task_t* task = w->task;
    poll_waitq_t* q = w->q;

    if (q) {
        if (!poll_waitq_try_unlink_node(&w->q_node)) {
        }

        w->q = nullptr;
    }

    if (task) {
        if (!poll_waitq_try_unlink_node(&w->task_node)) {
        }

        w->task = nullptr;
        proc_task_put(task);
    }

    if (q) {
        poll_waitq_put(q);
    }

    spinlock_release_safe(&g_poll_global_lock, flags);
}

void poll_waitq_wake_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t flags = spinlock_acquire_safe(&g_poll_global_lock);

    for (dlist_head_t* it = q->waiters.next; it != &q->waiters; it = it->next) {
        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);
        task_t* task = w->task;
        if (task) {
            proc_wake(task);
        }
    }

    spinlock_release_safe(&g_poll_global_lock, flags);
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t flags = spinlock_acquire_safe(&g_poll_global_lock);

    while (!dlist_empty(&q->waiters)) {
        poll_waiter_t* w = container_of(q->waiters.next, poll_waiter_t, q_node);
        task_t* task = w->task;

        if (task) {
            if (!poll_waitq_try_unlink_node(&w->task_node)) {
            }

            w->task = nullptr;
            proc_task_put(task);
        }

        if (!poll_waitq_try_unlink_node(&w->q_node)) {
        }

        w->q = nullptr;

        poll_waitq_put(q);
    }

    spinlock_release_safe(&g_poll_global_lock, flags);
}

void poll_task_cleanup(struct task* task) {
    if (!task) return;

    uint32_t flags = spinlock_acquire_safe(&g_poll_global_lock);

    while (!dlist_empty(&task->poll_waiters)) {
        poll_waiter_t* w = container_of(task->poll_waiters.next, poll_waiter_t, task_node);
        poll_waitq_t* q = w->q;

        if (q) {
            if (!poll_waitq_try_unlink_node(&w->q_node)) {
            }

            w->q = nullptr;
        }

        if (!poll_waitq_try_unlink_node(&w->task_node)) {
        }
        
        if (w->task) {
            w->task = nullptr;
            proc_task_put(task);
        }

        if (q) {
            poll_waitq_put(q);
        }
    }

    spinlock_release_safe(&g_poll_global_lock, flags);
}

}

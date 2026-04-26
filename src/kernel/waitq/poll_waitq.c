/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <lib/compiler.h>
#include <lib/atomic.h>

#include <kernel/panic.h>
#include <kernel/proc.h>

#include <hal/irq.h>
#include <hal/cpu.h>

#include "poll_waitq.h"

static int poll_waitq_waiter_is_clean(const poll_waiter_t* w) {
    if (unlikely(!w)) {
        panic("poll_waitq_waiter_is_clean: called on null poll_waiter_t*\n");
    }

    if (unlikely(w->task != 0
        || w->q != 0)) {
        return 0;
    }

    if (unlikely(w->q_node.next != 0
        || w->q_node.prev != 0)) {
        return 0;
    }

    if (unlikely(w->task_node.next != 0
        || w->task_node.prev != 0)) {
        return 0;
    }

    return 1;
}

static int poll_waitq_try_unlink_node(dlist_head_t* node) {
    if (unlikely(!node)) {
        return 0;
    }

    return dlist_unlink_consistent(node);
}

void poll_waitq_init(poll_waitq_t* q) {
    if (unlikely(!q)) {
        panic("poll_waitq_init: called on null poll_waitq_t*\n");
    }
    
    spinlock_init(&q->lock);

    dlist_init(&q->waiters);

    q->refs = 1u;
    q->finalize = 0;
    q->finalize_ctx = 0;
}

void poll_waitq_init_finalizable(poll_waitq_t* q, void (*finalize)(void* ctx),void* finalize_ctx) {
    if (unlikely(!q)) {
        panic("poll_waitq_init_finalizable: called on null poll_waitq_t*\n");
    }

    poll_waitq_init(q);

    q->finalize = finalize;
    q->finalize_ctx = finalize_ctx;
}

void poll_waitq_retain(poll_waitq_t* q) {
    if (unlikely(!q)) {
        panic("poll_waitq_retain: called on null poll_waitq_t*\n");
    }

    __atomic_fetch_add(&q->refs, 1u, __ATOMIC_RELAXED);
}

void poll_waitq_put(poll_waitq_t* q) {
    if (unlikely(!q)) {
        panic("poll_waitq_put: called on null poll_waitq_t*\n");
    }

    uint32_t old_refs = __atomic_fetch_sub(&q->refs, 1u, __ATOMIC_ACQ_REL);

    if (unlikely(old_refs == 0u)) {
        panic("POLL: waitq_put underflow");
    }

    if (unlikely(old_refs == 1u)) {
        void (*finalize)(void*) = q->finalize;
        void* finalize_ctx = q->finalize_ctx;

        if (unlikely(!finalize)) {
            panic("POLL: waitq finalized without finalizer\n");
        }

        finalize(finalize_ctx);
    }
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task) {
    if (unlikely(!q || !w || !task)) {
        panic("poll_waitq_register: some of arguments are null\n");
    }

    guard(spinlock_safe)(&q->lock);

    if (unlikely(!poll_waitq_waiter_is_clean(w))) {
        return -1;
    }

    if (unlikely(!proc_task_retain(task))) {
        return -1;
    }

    poll_waitq_retain(q);

    {
        guard(spinlock)(&task->poll_lock);

        w->task = task;
        w->q = q;

        atomic_uint_store_explicit(&w->triggered_events, 0, ATOMIC_RELAXED);

        dlist_add_tail(&w->q_node, &q->waiters);
        dlist_add_tail(&w->task_node, &task->poll_waiters);
    }

    return 0;
}

static int poll_waitq_do_unregister(struct task* task, poll_waiter_t* target_w) {
    uint32_t flags = irq_save();

    poll_waitq_t* q_to_put = 0;
    
    int status = 1;

    {
        guard(spinlock)(&task->poll_lock);

        poll_waiter_t* w = target_w;
        
        if (!w) {
            if (dlist_empty(&task->poll_waiters)) {
                status = -1;
            } else {
                w = container_of(task->poll_waiters.next, poll_waiter_t, task_node);
            }
        }

        if (w) {
            poll_waitq_t* q = w->q;

            if (!q) {
                if (w->task) {
                    w->task = 0;

                    proc_task_put(task);
                }

                poll_waitq_try_unlink_node(&w->task_node);
            } else if (unlikely(!spinlock_try_acquire(&q->lock))) {
                status = 0;
            } else {
                if (poll_waitq_try_unlink_node(&w->q_node)) {
                    w->q = 0;
                }

                if (poll_waitq_try_unlink_node(&w->task_node)) {
                    w->task = 0;
                    proc_task_put(task);
                }

                spinlock_release(&q->lock);
                
                q_to_put = q;
            }
        }
    }

    irq_restore(flags);

    if (q_to_put) {
        poll_waitq_put(q_to_put);
    }

    return status;
}

void poll_waitq_unregister(poll_waiter_t* w) {
    if (unlikely(!w)) {
        panic("poll_waitq_unregister: called on null poll_waiter_t*\n");
    }

    task_t* task = w->task;

    if (!task) {
        task = proc_current();
    }

    while (poll_waitq_do_unregister(task, w) == 0) {
        cpu_relax();
    }
}

void poll_waitq_wake_all(poll_waitq_t* q, uint32_t events) {
    if (unlikely(!q)) {
        panic("poll_waitq_wake_all: called on null poll_waitq_t*\n");
    }

    guard(spinlock_safe)(&q->lock);

    for (dlist_head_t* it = q->waiters.next; it != &q->waiters; it = it->next) {
        
        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);

        task_t* task = w->task;

        atomic_uint_fetch_or_explicit(&w->triggered_events, events, ATOMIC_RELEASE);

        if (likely(task != 0)) {
            proc_wake(task);
        }
    }
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (unlikely(!q)) {
        panic("poll_waitq_detach_all: called on null poll_waitq_t*\n");
    }

    guard(spinlock_safe)(&q->lock);

    while (!dlist_empty(&q->waiters)) {
        poll_waiter_t* w = container_of(q->waiters.next, poll_waiter_t, q_node);

        task_t* task = w->task;

        if (task) {
            guard(spinlock)(&task->poll_lock);

            if (poll_waitq_try_unlink_node(&w->task_node)) {
                w->task = 0;

                proc_task_put(task);
            }
        }

        if (poll_waitq_try_unlink_node(&w->q_node)) {
            w->q = 0;
        }

        poll_waitq_put(q);
    }
}

void poll_task_cleanup(struct task* task) {
    if (unlikely(!task)) {
        panic("poll_task_cleanup: called on null struct task*\n");
    }

    for (;;) {
        int status = poll_waitq_do_unregister(task, 0);

        if (status == -1) {
            break;
        }

        if (status == 0) {
            cpu_relax();
        }
    }
}
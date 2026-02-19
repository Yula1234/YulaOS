#include "poll_waitq.h"

#include <arch/i386/paging.h>

#include <kernel/panic.h>
#include <kernel/proc.h>

#include <lib/cpp/lock_guard.h>

#include <stdint.h>

namespace {

static inline bool poll_ptr_mapped(const void* p) {
    if (!p) {
        return false;
    }

    const uint32_t v = (uint32_t)p;

    return paging_get_phys(kernel_page_directory, v) != 0u;
}

static inline int poll_dlist_node_valid(const dlist_head_t* node) {
    return poll_ptr_mapped(node) ? 1 : 0;
}

static inline void poll_on_corrupt(const char* msg) {
    panic(msg);
}

}

extern "C" {

void poll_waitq_init(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    spinlock_init(&q->lock);

    dlist_init(&q->waiters);
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task) {
    if (!q || !w || !task) {
        return -1;
    }

    kernel::SpinLockNativeSafeGuard task_guard(task->poll_lock);

    if (w->task
        || w->q
        || dlist_node_linked(&w->q_node)
        || dlist_node_linked(&w->task_node)) {
        return -1;
    }

    kernel::SpinLockNativeSafeGuard q_guard(q->lock);

    w->task = task;
    w->q = q;

    dlist_add_tail(&w->q_node, &q->waiters);
    dlist_add_tail(&w->task_node, &task->poll_waiters);

    return 0;
}

void poll_waitq_unregister(poll_waiter_t* w) {
    if (!w) {
        return;
    }

    if (!poll_ptr_mapped(w)) {
        panic("POLL: waiter unmapped");
    }

    task_t* task = (task_t*)w->task;

    if (!task) {
        w->task = 0;
        w->q = 0;

        return;
    }

    if (!poll_ptr_mapped(task)) {
        panic("POLL: waiter->task unmapped");
    }

    kernel::SpinLockNativeSafeGuard task_guard(task->poll_lock);

    if (w->task != task) {
        return;
    }

    poll_waitq_t* q = w->q;

    if (q && !poll_ptr_mapped(q)) {
        panic("POLL: waiter->q unmapped");
    }

    (void)dlist_remove_node_if_present_checked(
        &task->poll_waiters,
        &w->task_node,
        poll_dlist_node_valid,
        poll_on_corrupt
    );

    if (!q) {
        w->task = 0;
        w->q = 0;

        return;
    }

    kernel::SpinLockNativeSafeGuard q_guard(q->lock);

    if (w->q != q) {
        return;
    }

    (void)dlist_remove_node_if_present_checked(
        &q->waiters,
        &w->q_node,
        poll_dlist_node_valid,
        poll_on_corrupt
    );

    w->task = 0;
    w->q = 0;
}

void poll_waitq_wake_all(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    kernel::SpinLockNativeSafeGuard q_guard(q->lock);

    dlist_head_t* it = q->waiters.next;

    while (it && it != &q->waiters) {
        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);
        it = it->next;

        if (w->task) {
            proc_wake(w->task);
        }
    }
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    for (;;) {
        poll_waiter_t* w = 0;
        struct task* task = 0;

        {
            kernel::SpinLockNativeSafeGuard q_guard(q->lock);

            if (!dlist_empty(&q->waiters)) {
                w = container_of(q->waiters.next, poll_waiter_t, q_node);
                task = (struct task*)w->task;
            }
        }

        if (!w) {
            return;
        }

        if (!task) {
            kernel::SpinLockNativeSafeGuard q_guard(q->lock);

            if (w->q == q) {
                (void)dlist_remove_node_if_present_checked(
                    &q->waiters,
                    &w->q_node,
                    poll_dlist_node_valid,
                    poll_on_corrupt
                );

                w->q = 0;
            }

            continue;
        }

        kernel::SpinLockNativeSafeGuard task_guard(task->poll_lock);

        if (w->task != task || w->q != q) {
            continue;
        }

        {
            kernel::SpinLockNativeSafeGuard q_guard(q->lock);

            if (w->q == q) {
                (void)dlist_remove_node_if_present_checked(
                    &q->waiters,
                    &w->q_node,
                    poll_dlist_node_valid,
                    poll_on_corrupt
                );
            }

            (void)dlist_remove_node_if_present_checked(
                &task->poll_waiters,
                &w->task_node,
                poll_dlist_node_valid,
                poll_on_corrupt
            );

            w->q = 0;
            w->task = 0;
        }

        proc_wake(task);
    }
}

void poll_task_cleanup(struct task* task) {
    if (!task) {
        return;
    }

    kernel::SpinLockNativeSafeGuard task_guard(task->poll_lock);

    while (!dlist_empty(&task->poll_waiters)) {
        poll_waiter_t* w = container_of(task->poll_waiters.next, poll_waiter_t, task_node);

        poll_waitq_t* q = w->q;

        (void)dlist_remove_node_if_present_checked(
            &task->poll_waiters,
            &w->task_node,
            poll_dlist_node_valid,
            poll_on_corrupt
        );

        if (!q) {
            w->task = 0;
            w->q = 0;

            continue;
        }

        kernel::SpinLockNativeSafeGuard q_guard(q->lock);

        (void)dlist_remove_node_if_present_checked(
            &q->waiters,
            &w->q_node,
            poll_dlist_node_valid,
            poll_on_corrupt
        );

        w->task = 0;
        w->q = 0;
    }
}

}

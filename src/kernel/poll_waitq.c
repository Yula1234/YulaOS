// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "poll_waitq.h"

#include <kernel/proc.h>

#include <kernel/panic.h>

#include <arch/i386/paging.h>

static inline int dlist_node_linked(const dlist_head_t* node) {
    return node && node->next && node->prev;
}

static inline int poll_ptr_mapped(const void* p) {
    if (!p) return 0;
    uint32_t v = (uint32_t)p;
    return paging_get_phys(kernel_page_directory, v) != 0u;
}

static inline int dlist_unlink_consistent(dlist_head_t* node) {
    if (!node || !node->prev || !node->next) {
        return 0;
    }

    dlist_head_t* prev = node->prev;
    dlist_head_t* next = node->next;

    if (!prev || !next) {
        return 0;
    }

    if (!poll_ptr_mapped(prev) || !poll_ptr_mapped(next)) {
        return 0;
    }

    if (prev->next != node || next->prev != node) {
        return 0;
    }

    prev->next = next;
    next->prev = prev;

    node->next = 0;
    node->prev = 0;
    return 1;
}

static inline int dlist_remove_node_if_present(dlist_head_t* head, dlist_head_t* node) {
    if (!head || !node) return 0;

    if (dlist_unlink_consistent(node)) {
        return 1;
    }

    dlist_head_t* it = head->next;
    while (it && it != head) {
        if (!poll_ptr_mapped(it)) {
            panic("POLL: corrupted dlist (unmapped iter)");
        }
        if (it == node) {
            dlist_head_t* prev = it->prev;
            dlist_head_t* next = it->next;

            if (prev && next) {
                if (!poll_ptr_mapped(prev) || !poll_ptr_mapped(next)) {
                    panic("POLL: corrupted dlist (unmapped links)");
                }
                next->prev = prev;
                prev->next = next;
            }

            it->next = 0;
            it->prev = 0;
            return 1;
        }

        dlist_head_t* nxt = it->next;
        if (nxt && nxt != head && !poll_ptr_mapped(nxt)) {
            panic("POLL: corrupted dlist (unmapped next)");
        }
        it = nxt;
    }

    return 0;
}

void poll_waitq_init(poll_waitq_t* q) {
    if (!q) return;
    spinlock_init(&q->lock);
    dlist_init(&q->waiters);
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, task_t* task) {
    if (!q || !w || !task) return -1;

    uint32_t task_flags = spinlock_acquire_safe(&task->poll_lock);

    if (w->task || w->q || dlist_node_linked(&w->q_node) || dlist_node_linked(&w->task_node)) {
        spinlock_release_safe(&task->poll_lock, task_flags);
        return -1;
    }

    uint32_t q_flags = spinlock_acquire_safe(&q->lock);

    w->task = task;
    w->q = q;

    dlist_add_tail(&w->q_node, &q->waiters);
    dlist_add_tail(&w->task_node, &task->poll_waiters);

    spinlock_release_safe(&q->lock, q_flags);
    spinlock_release_safe(&task->poll_lock, task_flags);
    return 0;
}

void poll_waitq_unregister(poll_waiter_t* w) {
    if (!w) return;

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

    uint32_t task_flags = spinlock_acquire_safe(&task->poll_lock);
    if (w->task != task) {
        spinlock_release_safe(&task->poll_lock, task_flags);
        return;
    }

    poll_waitq_t* q = w->q;

    if (q && !poll_ptr_mapped(q)) {
        spinlock_release_safe(&task->poll_lock, task_flags);
        panic("POLL: waiter->q unmapped");
    }

    (void)dlist_remove_node_if_present(&task->poll_waiters, &w->task_node);

    if (!q) {
        w->task = 0;
        w->q = 0;
        spinlock_release_safe(&task->poll_lock, task_flags);
        return;
    }

    uint32_t q_flags = spinlock_acquire_safe(&q->lock);

    if (w->q != q) {
        spinlock_release_safe(&q->lock, q_flags);
        spinlock_release_safe(&task->poll_lock, task_flags);
        return;
    }

    (void)dlist_remove_node_if_present(&q->waiters, &w->q_node);

    w->task = 0;
    w->q = 0;

    spinlock_release_safe(&q->lock, q_flags);
    spinlock_release_safe(&task->poll_lock, task_flags);
}

void poll_waitq_wake_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t flags = spinlock_acquire_safe(&q->lock);

    dlist_head_t* it = q->waiters.next;
    while (it && it != &q->waiters) {
        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);
        it = it->next;

        if (w->task) {
            proc_wake(w->task);
        }
    }

    spinlock_release_safe(&q->lock, flags);
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (!q) return;

    for (;;) {
        poll_waiter_t* w = 0;
        task_t* task = 0;

        uint32_t q_flags = spinlock_acquire_safe(&q->lock);
        if (!dlist_empty(&q->waiters)) {
            w = container_of(q->waiters.next, poll_waiter_t, q_node);
            task = (task_t*)w->task;
        }
        spinlock_release_safe(&q->lock, q_flags);

        if (!w) {
            return;
        }

        if (!task) {
            uint32_t q_flags2 = spinlock_acquire_safe(&q->lock);
            if (w->q == q) {
                (void)dlist_remove_node_if_present(&q->waiters, &w->q_node);
                w->q = 0;
            }
            spinlock_release_safe(&q->lock, q_flags2);
            continue;
        }

        uint32_t task_flags = spinlock_acquire_safe(&task->poll_lock);
        if (w->task != task || w->q != q) {
            spinlock_release_safe(&task->poll_lock, task_flags);
            continue;
        }

        uint32_t q_flags2 = spinlock_acquire_safe(&q->lock);
        if (w->q == q) {
            (void)dlist_remove_node_if_present(&q->waiters, &w->q_node);
        }
        (void)dlist_remove_node_if_present(&task->poll_waiters, &w->task_node);

        w->q = 0;
        w->task = 0;

        spinlock_release_safe(&q->lock, q_flags2);
        spinlock_release_safe(&task->poll_lock, task_flags);

        proc_wake(task);
    }
}

void poll_task_cleanup(task_t* task) {
    if (!task) return;

    uint32_t task_flags = spinlock_acquire_safe(&task->poll_lock);

    while (!dlist_empty(&task->poll_waiters)) {
        poll_waiter_t* w = container_of(task->poll_waiters.next, poll_waiter_t, task_node);

        poll_waitq_t* q = w->q;

        (void)dlist_remove_node_if_present(&task->poll_waiters, &w->task_node);

        if (!q) {
            w->task = 0;
            w->q = 0;
            continue;
        }

        uint32_t q_flags = spinlock_acquire_safe(&q->lock);

        (void)dlist_remove_node_if_present(&q->waiters, &w->q_node);

        w->task = 0;
        w->q = 0;

        spinlock_release_safe(&q->lock, q_flags);
    }

    spinlock_release_safe(&task->poll_lock, task_flags);
}

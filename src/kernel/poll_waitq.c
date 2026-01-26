// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "poll_waitq.h"

#include <kernel/proc.h>

static inline int dlist_node_linked(const dlist_head_t* node) {
    return node && node->next && node->prev;
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

    task_t* task = (task_t*)w->task;
    poll_waitq_t* q = w->q;

    if (!task) {
        w->task = 0;
        w->q = 0;
        return;
    }

    uint32_t task_flags = spinlock_acquire_safe(&task->poll_lock);
    if (w->task != task) {
        spinlock_release_safe(&task->poll_lock, task_flags);
        return;
    }

    if (dlist_node_linked(&w->task_node)) {
        dlist_del(&w->task_node);
        w->task_node.next = 0;
        w->task_node.prev = 0;
    }

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

    if (dlist_node_linked(&w->q_node)) {
        dlist_del(&w->q_node);
        w->q_node.next = 0;
        w->q_node.prev = 0;
    }

    w->task = 0;
    w->q = 0;

    spinlock_release_safe(&q->lock, q_flags);
    spinlock_release_safe(&task->poll_lock, task_flags);
}

void poll_waitq_wake_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t flags = spinlock_acquire_safe(&q->lock);

    poll_waiter_t* w;
    dlist_for_each_entry(w, &q->waiters, q_node) {
        if (w->task) proc_wake(w->task);
    }

    spinlock_release_safe(&q->lock, flags);
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (!q) return;

    uint32_t flags = spinlock_acquire_safe(&q->lock);

    while (!dlist_empty(&q->waiters)) {
        poll_waiter_t* w = container_of(q->waiters.next, poll_waiter_t, q_node);

        dlist_del(&w->q_node);
        w->q_node.next = 0;
        w->q_node.prev = 0;

        w->q = 0;

        if (w->task) proc_wake(w->task);
    }

    spinlock_release_safe(&q->lock, flags);
}

void poll_task_cleanup(task_t* task) {
    if (!task) return;

    uint32_t task_flags = spinlock_acquire_safe(&task->poll_lock);

    while (!dlist_empty(&task->poll_waiters)) {
        poll_waiter_t* w = container_of(task->poll_waiters.next, poll_waiter_t, task_node);

        poll_waitq_t* q = w->q;

        if (dlist_node_linked(&w->task_node)) {
            dlist_del(&w->task_node);
            w->task_node.next = 0;
            w->task_node.prev = 0;
        }

        if (!q) {
            w->task = 0;
            w->q = 0;
            continue;
        }

        uint32_t q_flags = spinlock_acquire_safe(&q->lock);

        if (w->q == q && dlist_node_linked(&w->q_node)) {
            dlist_del(&w->q_node);
            w->q_node.next = 0;
            w->q_node.prev = 0;
        }

        w->task = 0;
        w->q = 0;

        spinlock_release_safe(&q->lock, q_flags);
    }

    spinlock_release_safe(&task->poll_lock, task_flags);
}

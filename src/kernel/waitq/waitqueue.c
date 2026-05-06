/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/waitq/waitqueue.h>

#include <kernel/proc.h>

#include <kernel/panic.h>
#include <kernel/sched.h>

#include <lib/compiler.h>

static inline void waitqueue_reset_task_wait_state(task_t* task) {
    task->sem_node.next = 0;
    task->sem_node.prev = 0;

    task->blocked_on_sem = 0;
    task->blocked_kind = TASK_BLOCK_NONE;
}

void waitqueue_init(waitqueue_t* q, void* blocked_on, task_block_kind_t kind) {
    if (unlikely(!q)) {
        return;
    }

    dlist_init(&q->waiters);

    q->blocked_on = blocked_on;
    q->kind = kind;
}

int waitqueue_wait_prepare_locked(
    waitqueue_t* q,
    task_t* task
) {
    if (unlikely(!q
        || !task)) {
        return -1;
    }

    if (unlikely(task->sem_node.next != 0
        || task->sem_node.prev != 0)) {
        panic("WAITQ: task is already linked\n");
    }

    task->blocked_on_sem = q->blocked_on;
    task->blocked_kind = q->kind;

    dlist_add_tail(&task->sem_node, &q->waiters);

    if (unlikely(proc_change_state(task, TASK_WAITING) != 0)) {
        dlist_del(&task->sem_node);

        waitqueue_reset_task_wait_state(task);
        return -1;
    }

    return 0;
}

void waitqueue_wait_cancel_locked(
    waitqueue_t* q,
    task_t* task
) {
    if (unlikely(!q
        || !task)) {
        return;
    }

    if (task->blocked_on_sem != q->blocked_on
        || task->blocked_kind != q->kind) {
        return;
    }

    if (dlist_unlink_consistent(&task->sem_node)) {
        waitqueue_reset_task_wait_state(task);
    }
}

task_t* waitqueue_dequeue_locked(waitqueue_t* q) {
    if (unlikely(!q)) {
        return 0;
    }

    if (dlist_empty(&q->waiters)) {
        return 0;
    }

    task_t* task = container_of(q->waiters.next, task_t, sem_node);

    dlist_del(&task->sem_node);

    waitqueue_reset_task_wait_state(task);

    return task;
}

void waitqueue_detach_all_locked(waitqueue_t* q, dlist_head_t* out_list) {
    if (unlikely(!q
        || !out_list)) {
        return;
    }

    dlist_init(out_list);

    if (dlist_empty(&q->waiters)) {
        return;
    }

    out_list->next = q->waiters.next;
    out_list->prev = q->waiters.prev;

    out_list->next->prev = out_list;
    out_list->prev->next = out_list;

    dlist_init(&q->waiters);
}

task_t* waitqueue_detached_list_pop(dlist_head_t* list) {
    if (unlikely(!list)) {
        return 0;
    }

    if (dlist_empty(list)) {
        return 0;
    }

    task_t* task = container_of(list->next, task_t, sem_node);

    dlist_del(&task->sem_node);

    waitqueue_reset_task_wait_state(task);

    return task;
}

void waitqueue_wake_detached_list(dlist_head_t* list) {
    if (unlikely(!list)) {
        return;
    }

    for (;;) {
        task_t* task = waitqueue_detached_list_pop(list);
        if (!task) {
            return;
        }

        proc_wake(task);
    }
}

void waitqueue_wake_task(task_t* task) {
    proc_wake(task);
}

int waitqueue_wake_one_locked(waitqueue_t* q) {
    if (unlikely(!q)) {
        return 0;
    }

    for (;;) {
        task_t* task = waitqueue_dequeue_locked(q);
        if (!task) {
            return 0;
        }

        proc_wake(task);
        return 1;
    }
}

uint32_t waitqueue_wake_all_locked(waitqueue_t* q) {
    if (unlikely(!q)) {
        return 0;
    }

    uint32_t woken = 0;

    while (!dlist_empty(&q->waiters)) {
        task_t* task = waitqueue_dequeue_locked(q);
        if (!task) {
            break;
        }

        proc_wake(task);
        woken++;
    }

    return woken;
}

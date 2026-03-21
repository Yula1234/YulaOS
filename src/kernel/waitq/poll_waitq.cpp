// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "poll_waitq.h"
#include <kernel/proc.h>
#include <kernel/output/kprintf.h>
#include <hal/lock.h>

extern "C" {

static spinlock_t g_poll_global_lock = {0};

#define POLL_WAITQ_DIAG 1

#define POLL_WAITQ_DIAG_MAX_PER_EVENT 32u

enum class PollWaitqDiagEvent : uint32_t {
    RegisterNotClean,
    RegisterRetainFailed,
    UnregisterQNodeUnlinkFailed,
    UnregisterTaskNodeUnlinkFailed,
    DetachAllQNodeUnlinkFailed,
    DetachAllTaskNodeUnlinkFailed,
    TaskCleanupQNodeUnlinkFailed,
    TaskCleanupTaskNodeUnlinkFailed,
    Count,
};

static uint32_t g_poll_waitq_diag_counts[(uint32_t)PollWaitqDiagEvent::Count] = {};

static int poll_waitq_diag_should_log(PollWaitqDiagEvent ev) {
    const uint32_t idx = (uint32_t)ev;

    if (idx >= (uint32_t)PollWaitqDiagEvent::Count) {
        return 0;
    }

    uint32_t& c = g_poll_waitq_diag_counts[idx];

    if (c >= POLL_WAITQ_DIAG_MAX_PER_EVENT) {
        return 0;
    }

    c++;
    return 1;
}

static void poll_waitq_diag_print_waiter(const char* tag, const poll_waiter_t* w) {
    if (!w) {
        kprintf("[poll_waitq] %s w=null\n", tag ? tag : "?");
        return;
    }

    kprintf(
        "[poll_waitq] %s w=%p task=%p q=%p qn(prev=%p,next=%p) tn(prev=%p,next=%p)\n",
        tag ? tag : "?",
        w,
        w->task,
        w->q,
        w->q_node.prev,
        w->q_node.next,
        w->task_node.prev,
        w->task_node.next
    );
}

static void poll_waitq_diag_print_queue(const char* tag, const poll_waitq_t* q) {
    if (!q) {
        kprintf("[poll_waitq] %s q=null\n", tag ? tag : "?");
        return;
    }

    const dlist_head_t* head = &q->waiters;
    kprintf(
        "[poll_waitq] %s q=%p head=%p head(prev=%p,next=%p)\n",
        tag ? tag : "?",
        q,
        head,
        head->prev,
        head->next
    );
}

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
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task) {
    if (!q || !w || !task) return -1;

    uint32_t flags = spinlock_acquire_safe(&g_poll_global_lock);

    if (!poll_waitq_waiter_is_clean(w)) {
#if POLL_WAITQ_DIAG
        if (poll_waitq_diag_should_log(PollWaitqDiagEvent::RegisterNotClean)) {
            poll_waitq_diag_print_waiter("register: waiter not clean", w);
            poll_waitq_diag_print_queue("register: queue", q);
            kprintf("[poll_waitq] register: task=%p pid=%u\n", task, task->pid);
        }
#endif
        spinlock_release_safe(&g_poll_global_lock, flags);
        return -1;
    }

    if (!proc_task_retain(task)) {
#if POLL_WAITQ_DIAG
        if (poll_waitq_diag_should_log(PollWaitqDiagEvent::RegisterRetainFailed)) {
            poll_waitq_diag_print_waiter("register: task retain failed", w);
            poll_waitq_diag_print_queue("register: queue", q);
            kprintf("[poll_waitq] register: task=%p pid=%u\n", task, task->pid);
        }
#endif
        spinlock_release_safe(&g_poll_global_lock, flags);
        return -1;
    }

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
#if POLL_WAITQ_DIAG
            if (poll_waitq_diag_should_log(PollWaitqDiagEvent::UnregisterQNodeUnlinkFailed)) {
                poll_waitq_diag_print_waiter("unregister: q_node unlink failed", w);
                poll_waitq_diag_print_queue("unregister: queue", q);
            }
#endif
        }

        w->q = nullptr;
    }

    if (task) {
        if (!poll_waitq_try_unlink_node(&w->task_node)) {
#if POLL_WAITQ_DIAG
            if (poll_waitq_diag_should_log(PollWaitqDiagEvent::UnregisterTaskNodeUnlinkFailed)) {
                poll_waitq_diag_print_waiter("unregister: task_node unlink failed", w);
                kprintf("[poll_waitq] unregister: task=%p pid=%u\n", task, task->pid);
            }
#endif
        }

        w->task = nullptr;
        proc_task_put(task);
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
#if POLL_WAITQ_DIAG
                if (poll_waitq_diag_should_log(PollWaitqDiagEvent::DetachAllTaskNodeUnlinkFailed)) {
                    poll_waitq_diag_print_waiter("detach_all: task_node unlink failed", w);
                    kprintf("[poll_waitq] detach_all: task=%p pid=%u\n", task, task->pid);
                }
#endif
            }

            w->task = nullptr;
            proc_task_put(task);
        }

        if (!poll_waitq_try_unlink_node(&w->q_node)) {
#if POLL_WAITQ_DIAG
            if (poll_waitq_diag_should_log(PollWaitqDiagEvent::DetachAllQNodeUnlinkFailed)) {
                poll_waitq_diag_print_waiter("detach_all: q_node unlink failed", w);
                poll_waitq_diag_print_queue("detach_all: queue", q);
            }
#endif
        }

        w->q = nullptr;
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
#if POLL_WAITQ_DIAG
                if (poll_waitq_diag_should_log(PollWaitqDiagEvent::TaskCleanupQNodeUnlinkFailed)) {
                    poll_waitq_diag_print_waiter("task_cleanup: q_node unlink failed", w);
                    poll_waitq_diag_print_queue("task_cleanup: queue", q);
                    kprintf("[poll_waitq] task_cleanup: task=%p pid=%u\n", task, task->pid);
                }
#endif
            }

            w->q = nullptr;
        }

        if (!poll_waitq_try_unlink_node(&w->task_node)) {
#if POLL_WAITQ_DIAG
            if (poll_waitq_diag_should_log(PollWaitqDiagEvent::TaskCleanupTaskNodeUnlinkFailed)) {
                poll_waitq_diag_print_waiter("task_cleanup: task_node unlink failed", w);
                kprintf("[poll_waitq] task_cleanup: task=%p pid=%u\n", task, task->pid);
            }
#endif
        }
        
        if (w->task) {
            w->task = nullptr;
            proc_task_put(task);
        }
    }

    spinlock_release_safe(&g_poll_global_lock, flags);
}

}

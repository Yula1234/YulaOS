/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/workqueue.h>

#include <kernel/proc.h>
#include <kernel/sched.h>

#include <mm/heap.h>

struct WorkQueue {
    work_struct_t* head_;
    semaphore_t sem_;

    task_t* worker_task_;
    uint32_t dying_;
};

static work_struct_t* reverse_work_list(work_struct_t* list) {
    work_struct_t* prev = 0;
    work_struct_t* curr = list;

    while (curr) {
        work_struct_t* next = curr->next_;

        curr->next_ = prev;
        prev = curr;
        curr = next;
    }

    return prev;
}

static void workqueue_worker_task(void* arg) {
    workqueue_t* wq = (workqueue_t*)arg;

    while (1) {
        sem_wait(&wq->sem_);

        uint32_t dying = __atomic_load_n(&wq->dying_, __ATOMIC_ACQUIRE);

        if (dying) {
            break;
        }

        work_struct_t* list = __atomic_exchange_n(&wq->head_, 0, __ATOMIC_ACQ_REL);

        if (!list) {
            continue;
        }

        work_struct_t* reversed = reverse_work_list(list);

        while (reversed) {
            work_struct_t* work = reversed;
            reversed = reversed->next_;

            __atomic_store_n(&work->pending_, 0, __ATOMIC_RELEASE);

            if (work->func_) {
                work->func_(work);
            }
        }
    }
}

workqueue_t* create_workqueue(const char* name) {
    if (!name) {
        return 0;
    }

    workqueue_t* wq = (workqueue_t*)kmalloc(sizeof(workqueue_t));

    if (!wq) {
        return 0;
    }

    wq->head_ = 0;
    wq->dying_ = 0;

    sem_init(&wq->sem_, 0);

    wq->worker_task_ = proc_spawn_kthread(
        name,
        PRIO_HIGH,
        workqueue_worker_task,
        wq
    );

    if (!wq->worker_task_) {
        kfree(wq);

        return 0;
    }

    return wq;
}

void destroy_workqueue(workqueue_t* wq) {
    if (!wq) {
        return;
    }

    __atomic_store_n(&wq->dying_, 1, __ATOMIC_RELEASE);

    sem_signal(&wq->sem_);

    if (wq->worker_task_) {
        proc_wait(wq->worker_task_->pid);
    }

    kfree(wq);
}

int queue_work(workqueue_t* wq, work_struct_t* work) {
    if (!wq
        || !work) {
        return 0;
    }

    uint32_t expected = 0;

    if (!__atomic_compare_exchange_n(
            &work->pending_, &expected,
            1, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE
        )) {
        return 0;
    }

    work_struct_t* old_head = __atomic_load_n(&wq->head_, __ATOMIC_ACQUIRE);

    do {
        work->next_ = old_head;
    } while (!__atomic_compare_exchange_n(
        &wq->head_, &old_head, work, 1,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED
    ));

    sem_signal(&wq->sem_);

    return 1;
}
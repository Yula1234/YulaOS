/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/workqueue.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <hal/align.h>
#include <hal/cpu.h>

#include <mm/heap.h>

struct WorkQueue {
    work_struct_t* volatile head_ __cacheline_aligned;

    work_struct_t* tail_ __cacheline_aligned;
    work_struct_t stub_;

    volatile uint32_t sleeping_ __cacheline_aligned;

    semaphore_t sem_ __cacheline_aligned;

    task_t* worker_task_;

    volatile uint32_t dying_;
};

___inline void mpsc_push(workqueue_t* wq, work_struct_t* work) {
    WRITE_ONCE(work->next_, 0);

    work_struct_t* prev = __atomic_exchange_n(
        &wq->head_, work, __ATOMIC_ACQ_REL
    );

    __atomic_store_n(&prev->next_, work, __ATOMIC_RELEASE);
}

static work_struct_t* mpsc_pop(workqueue_t* wq) {
    work_struct_t* tail = wq->tail_;
    work_struct_t* next = __atomic_load_n(&tail->next_, __ATOMIC_ACQUIRE);

    if (unlikely(tail == &wq->stub_)) {
        if (!next) {
            return 0;
        }

        wq->tail_ = next;
        tail = next;
        next = __atomic_load_n(&tail->next_, __ATOMIC_ACQUIRE);
    }

    if (likely(next)) {
        wq->tail_ = next;

        __builtin_prefetch(next, 0, 0);

        return tail;
    }

    work_struct_t* head = __atomic_load_n(&wq->head_, __ATOMIC_ACQUIRE);

    if (tail != head) {
        /*
         * A producer is currently pushing a node but hasn't linked it yet.
         * Spin gently until the next_ pointer becomes visible.
         */
        while (!next) {
            cpu_relax();
            next = __atomic_load_n(&tail->next_, __ATOMIC_ACQUIRE);
        }

        wq->tail_ = next;

        __builtin_prefetch(next, 0, 0);

        return tail;
    }

    /*
     * The queue is genuinely empty. Inject the stub node to securely
     * reset the queue state and catch the tail.
     */
    WRITE_ONCE(wq->stub_.next_, 0);

    work_struct_t* prev = __atomic_exchange_n(
        &wq->head_, &wq->stub_, __ATOMIC_ACQ_REL
    );

    __atomic_store_n(&prev->next_, &wq->stub_, __ATOMIC_RELEASE);

    next = __atomic_load_n(&tail->next_, __ATOMIC_ACQUIRE);

    if (likely(next)) {
        wq->tail_ = next;

        __builtin_prefetch(next, 0, 0);

        return tail;
    }

    return 0;
}

static void workqueue_worker_task(void* arg) {
    workqueue_t* wq = (workqueue_t*)arg;

    for (;;) {
        work_struct_t* work = mpsc_pop(wq);

        if (likely(work)) {
            work_func_t func = work->func_;

            __atomic_store_n(&work->pending_, 0, __ATOMIC_RELEASE);

            WRITE_ONCE(work->executing_, 1);

            if (likely(func)) {
                func(work);
            }

            __atomic_store_n(&work->executing_, 0, __ATOMIC_RELEASE);

            continue;
        }

        /*
         * The queue appears empty. Announce the intention to sleep
         * so producers know they must emit a wakeup signal.
         */
        __atomic_store_n(&wq->sleeping_, 1, __ATOMIC_SEQ_CST);

        work = mpsc_pop(wq);

        if (unlikely(work)) {
            /*
             * A task snuck in right as we were going to sleep.
             * Abort the sleep and process it immediately.
             */
            __atomic_store_n(&wq->sleeping_, 0, __ATOMIC_SEQ_CST);

            work_func_t func = work->func_;

            __atomic_store_n(&work->pending_, 0, __ATOMIC_RELEASE);

            WRITE_ONCE(work->executing_, 1);

            if (likely(func)) {
                func(work);
            }

            __atomic_store_n(&work->executing_, 0, __ATOMIC_RELEASE);

            continue;
        }

        uint32_t dying = __atomic_load_n(&wq->dying_, __ATOMIC_ACQUIRE);

        if (unlikely(dying)) {
            break;
        }

        sem_wait(&wq->sem_);
    }
}

workqueue_t* create_workqueue(const char* name) {
    if (unlikely(!name)) {
        return 0;
    }

    workqueue_t* wq = (workqueue_t*)kmalloc(sizeof(workqueue_t));

    if (unlikely(!wq)) {
        return 0;
    }

    memset(wq, 0, sizeof(*wq));

    wq->stub_.next_ = 0;
    wq->head_ = &wq->stub_;
    wq->tail_ = &wq->stub_;

    wq->sleeping_ = 0;
    wq->dying_ = 0;

    sem_init(&wq->sem_, 0);

    wq->worker_task_ = proc_spawn_kthread(
        name, PRIO_HIGH,
        workqueue_worker_task, wq
    );

    if (unlikely(!wq->worker_task_)) {
        kfree(wq);
        return 0;
    }

    return wq;
}

void destroy_workqueue(workqueue_t* wq) {
    if (unlikely(!wq)) {
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
    if (unlikely(!wq
        || !work)) {
        return 0;
    }

    const uint32_t already_pending = __atomic_exchange_n(
        &work->pending_, 1, __ATOMIC_ACQ_REL
    );

    if (unlikely(already_pending)) {
        return 0;
    }

    mpsc_push(wq, work);

    const uint32_t was_sleeping = __atomic_exchange_n(
        &wq->sleeping_, 0, __ATOMIC_SEQ_CST
    );

    if (likely(was_sleeping)) {
        sem_signal(&wq->sem_);
    }

    return 1;
}

void flush_work(work_struct_t* work) {
    if (unlikely(!work)) {
        return;
    }
    
    while (__atomic_load_n(&work->pending_, __ATOMIC_ACQUIRE) != 0 ||
           __atomic_load_n(&work->executing_, __ATOMIC_ACQUIRE) != 0) {
        cpu_relax();
        sched_yield();
    }
}
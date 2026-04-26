/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef KERNEL_WAITQ_POLL_WAITQ_H
#define KERNEL_WAITQ_POLL_WAITQ_H

#include <kernel/locking/spinlock.h>

#include <lib/atomic.h>
#include <lib/dlist.h>

#include <stdint.h>

struct task;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct poll_waitq {
    spinlock_t lock;
    dlist_head_t waiters;

    volatile uint32_t refs;

    void (*finalize)(void* ctx);
    void* finalize_ctx;
} poll_waitq_t;

typedef struct poll_waiter {
    struct task* task;
    poll_waitq_t* q;

    dlist_head_t q_node;
    dlist_head_t task_node;

    atomic_uint_t triggered_events;
} poll_waiter_t;

void poll_waitq_init(poll_waitq_t* q);

void poll_waitq_init_finalizable(
    poll_waitq_t* q,
    void (*finalize)(void* ctx),
    void* finalize_ctx
);

void poll_waitq_retain(poll_waitq_t* q);
void poll_waitq_put(poll_waitq_t* q);

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task);
void poll_waitq_unregister(poll_waiter_t* w);

void poll_waitq_wake_all(poll_waitq_t* q, uint32_t events);

void poll_waitq_detach_all(poll_waitq_t* q);

void poll_task_cleanup(struct task* task);

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_WAITQ_WAITQUEUE_H
#define KERNEL_WAITQ_WAITQUEUE_H

#include <kernel/proc_types.h>

#include <lib/dlist.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct waitqueue {
    dlist_head_t waiters;

    void* blocked_on;
    task_block_kind_t kind;
} waitqueue_t;

void waitqueue_init(waitqueue_t* q, void* blocked_on, task_block_kind_t kind);

int waitqueue_wait_prepare_locked(
    waitqueue_t* q,
    task_t* task
);

void waitqueue_wait_cancel_locked(
    waitqueue_t* q,
    task_t* task
);

task_t* waitqueue_dequeue_locked(waitqueue_t* q);

void waitqueue_detach_all_locked(waitqueue_t* q, dlist_head_t* out_list);

task_t* waitqueue_detached_list_pop(dlist_head_t* list);

void waitqueue_wake_detached_list(dlist_head_t* list);

int waitqueue_wake_one_locked(waitqueue_t* q);
uint32_t waitqueue_wake_all_locked(waitqueue_t* q);

void waitqueue_wake_task(task_t* task);

#ifdef __cplusplus
}
#endif

#endif

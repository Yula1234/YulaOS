/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_WORKQUEUE_H
#define KERNEL_WORKQUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WorkStruct;

typedef void (*work_func_t)(struct WorkStruct* work);

typedef struct WorkStruct {
    struct WorkStruct* next_;
    work_func_t func_;
    uint32_t pending_;
} work_struct_t;

static inline void init_work(work_struct_t* work, work_func_t func) {
    if (!work) {
        return;
    }

    work->next_ = 0;
    work->func_ = func;
    work->pending_ = 0;
}

struct WorkQueue;

typedef struct WorkQueue workqueue_t;

workqueue_t* create_workqueue(const char* name);

void destroy_workqueue(workqueue_t* wq);

int queue_work(workqueue_t* wq, work_struct_t* work);

#ifdef __cplusplus
}
#endif

#endif
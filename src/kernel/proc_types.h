/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_PROC_TYPES_H
#define KERNEL_PROC_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

struct task;

typedef struct task task_t;

typedef enum {
    TASK_BLOCK_NONE = 0,
    TASK_BLOCK_SEM = 1,
    TASK_BLOCK_FUTEX = 2,
} task_block_kind_t;

#ifdef __cplusplus
}
#endif

#endif

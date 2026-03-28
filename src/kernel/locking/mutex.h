/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef KERNEL_LOCKING_MUTEX_H
#define KERNEL_LOCKING_MUTEX_H

#include <kernel/locking/sem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    semaphore_t sem;
} mutex_t;

void mutex_init(mutex_t* m);
void mutex_lock(mutex_t* m);
void mutex_unlock(mutex_t* m);
int mutex_try_lock(mutex_t* m);

#ifdef __cplusplus
}
#endif

#endif

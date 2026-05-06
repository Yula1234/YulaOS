/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_MUTEX_H
#define KERNEL_LOCKING_MUTEX_H

#include <kernel/waitq/waitqueue.h>

#include <stdint.h>

#include "spinlock.h"
#include "guards.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uintptr_t owner;

    spinlock_t wait_lock;
    waitqueue_t waitq;
} mutex_t;

void mutex_init(mutex_t* m);

void mutex_lock(mutex_t* m);

void mutex_unlock(mutex_t* m);

int mutex_try_lock(mutex_t* m);

#ifndef __cplusplus
DEFINE_GUARD(mutex, mutex_t, mutex_lock, mutex_unlock)
#endif

#ifdef __cplusplus
}
#endif

#endif
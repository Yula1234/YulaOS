/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_MUTEX_H
#define KERNEL_LOCKING_MUTEX_H

#include <stdint.h>

#include <kernel/locking/spinlock.h>
#include <lib/dlist.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uintptr_t owner;

    spinlock_t wait_lock;
    dlist_head_t wait_list;
} mutex_t;

void mutex_init(mutex_t* m);

void mutex_lock(mutex_t* m);

void mutex_unlock(mutex_t* m);

int mutex_try_lock(mutex_t* m);

#ifdef __cplusplus
}
#endif

#endif
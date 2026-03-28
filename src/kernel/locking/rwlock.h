/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_RWLOCK_H
#define KERNEL_LOCKING_RWLOCK_H

#include <lib/dlist.h>

#include <stdint.h>

#include "spinlock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t state_;
    volatile uintptr_t writer_owner_;

    spinlock_t wait_lock_;

    dlist_head_t read_waiters_;
    dlist_head_t write_waiters_;
} rwlock_t;

void rwlock_init(rwlock_t* rw);

void rwlock_acquire_read(rwlock_t* rw);

void rwlock_release_read(rwlock_t* rw);

void rwlock_acquire_write(rwlock_t* rw);

void rwlock_release_write(rwlock_t* rw);

#ifdef __cplusplus
}
#endif

#endif
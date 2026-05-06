/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_RWLOCK_H
#define KERNEL_LOCKING_RWLOCK_H

#include <kernel/waitq/waitqueue.h>

#include <stdint.h>

#include "spinlock.h"
#include "guards.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t state_;
    volatile uintptr_t writer_owner_;

    spinlock_t wait_lock_;

    waitqueue_t read_waitq_;
    waitqueue_t write_waitq_;
} rwlock_t;

void rwlock_init(rwlock_t* rw);

void rwlock_acquire_read(rwlock_t* rw);

void rwlock_release_read(rwlock_t* rw);

void rwlock_acquire_write(rwlock_t* rw);

void rwlock_release_write(rwlock_t* rw);

#ifndef __cplusplus
DEFINE_GUARD(rwlock_read,  rwlock_t, rwlock_acquire_read,  rwlock_release_read)
DEFINE_GUARD(rwlock_write, rwlock_t, rwlock_acquire_write, rwlock_release_write)
#endif

#ifdef __cplusplus
}
#endif

#endif
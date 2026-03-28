// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_LOCKING_RWLOCK_H
#define KERNEL_LOCKING_RWLOCK_H

#include <kernel/locking/sem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    semaphore_t lock;
    semaphore_t write_sem;
    semaphore_t turnstile;
    int readers;
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

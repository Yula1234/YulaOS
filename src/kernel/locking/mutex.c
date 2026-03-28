// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <kernel/locking/mutex.h>

void mutex_init(mutex_t* m) {
    sem_init(&m->sem, 1);
}

void mutex_lock(mutex_t* m) {
    sem_wait(&m->sem);
}

void mutex_unlock(mutex_t* m) {
    sem_signal(&m->sem);
}

int mutex_try_lock(mutex_t* m) {
    return sem_try_acquire(&m->sem);
}

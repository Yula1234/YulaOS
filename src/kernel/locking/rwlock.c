// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <kernel/locking/rwlock.h>

void rwlock_init(rwlock_t* rw) {
    sem_init(&rw->lock, 1);
    sem_init(&rw->write_sem, 1);
    sem_init(&rw->turnstile, 1);
    rw->readers = 0;
}

void rwlock_acquire_read(rwlock_t* rw) {
    sem_wait(&rw->turnstile);
    sem_signal(&rw->turnstile);

    sem_wait(&rw->lock);
    rw->readers++;
    if (rw->readers == 1) {
        sem_wait(&rw->write_sem);
    }
    sem_signal(&rw->lock);
}

void rwlock_release_read(rwlock_t* rw) {
    sem_wait(&rw->lock);
    rw->readers--;
    if (rw->readers == 0) {
        sem_signal(&rw->write_sem);
    }
    sem_signal(&rw->lock);
}

void rwlock_acquire_write(rwlock_t* rw) {
    sem_wait(&rw->turnstile);
    sem_wait(&rw->write_sem);
}

void rwlock_release_write(rwlock_t* rw) {
    sem_signal(&rw->write_sem);
    sem_signal(&rw->turnstile);
}

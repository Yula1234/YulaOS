/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_SEM_H
#define KERNEL_LOCKING_SEM_H

#include <lib/dlist.h>

#include <hal/align.h>

#include <stdint.h>

#include "spinlock.h"

#ifdef __cplusplus
extern "C" {
#endif

struct task;

typedef struct {
    volatile int count;
    uint8_t pad[HAL_CACHELINE_SIZE - sizeof(int)];

    spinlock_t lock;
    dlist_head_t wait_list;
} semaphore_t;

void sem_init(semaphore_t* sem, int init_count);

void sem_reset(semaphore_t* sem, int value);

void sem_wait(semaphore_t* sem);

int sem_wait_timeout(semaphore_t* sem, uint32_t deadline_tick);

void sem_signal(semaphore_t* sem);

void sem_signal_all(semaphore_t* sem);

int sem_try_acquire(semaphore_t* sem);

void sem_remove_task(struct task* t);

#ifdef __cplusplus
}
#endif

#endif
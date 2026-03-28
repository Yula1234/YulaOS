/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_SEM_H
#define KERNEL_LOCKING_SEM_H

#include <stdint.h>

#include <kernel/locking/spinlock.h>
#include <lib/dlist.h>

#ifdef __cplusplus
extern "C" {
#endif

struct task;

/*
 * Modern Counting Semaphore Implementation.
 *
 * The count represents the number of available tokens. Tasks waiting for
 * a token to become available are enqueued in the wait_list.
 *
 * The fast-path relies on a lock-free atomic compare-and-swap. If the
 * fast-path fails, tasks perform a brief optimistic spin using cpu_relax()
 * before falling back to a lock-protected slow-path. This dramatically
 * reduces context switch overhead under light to moderate contention.
 */
typedef struct {
    volatile int count;

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
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_RWSPINLOCK_H
#define KERNEL_LOCKING_RWSPINLOCK_H

#include <kernel/smp/cpu_limits.h>

#include <hal/align.h>

#include <stdint.h>

#include "spinlock.h"
#include "guards.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t state;

    spinlock_t wait_lock;
} rwspinlock_t;

typedef struct {
    volatile uint32_t writer_active;
    
    spinlock_t writer_mutex;

    struct {
        volatile uint32_t count;

        uint8_t pad[HAL_CACHELINE_SIZE - sizeof(uint32_t)];
    } __cacheline_aligned readers[MAX_CPUS];
} percpu_rwspinlock_t;

void rwspinlock_init(rwspinlock_t* rw);

void rwspinlock_acquire_read(rwspinlock_t* rw);

void rwspinlock_release_read(rwspinlock_t* rw);

void rwspinlock_acquire_write(rwspinlock_t* rw);

void rwspinlock_release_write(rwspinlock_t* rw);


uint32_t rwspinlock_acquire_read_safe(rwspinlock_t* rw);

void rwspinlock_release_read_safe(rwspinlock_t* rw, uint32_t flags);

uint32_t rwspinlock_acquire_write_safe(rwspinlock_t* rw);

void rwspinlock_release_write_safe(rwspinlock_t* rw, uint32_t flags);


void percpu_rwspinlock_init(percpu_rwspinlock_t* rw);

void percpu_rwspinlock_acquire_read(percpu_rwspinlock_t* rw);

void percpu_rwspinlock_release_read(percpu_rwspinlock_t* rw);

void percpu_rwspinlock_acquire_write(percpu_rwspinlock_t* rw);

void percpu_rwspinlock_release_write(percpu_rwspinlock_t* rw);


uint32_t percpu_rwspinlock_acquire_read_safe(percpu_rwspinlock_t* rw);

void percpu_rwspinlock_release_read_safe(percpu_rwspinlock_t* rw, uint32_t flags);

uint32_t percpu_rwspinlock_acquire_write_safe(percpu_rwspinlock_t* rw);

void percpu_rwspinlock_release_write_safe(percpu_rwspinlock_t* rw, uint32_t flags);

#ifndef __cplusplus
DEFINE_GUARD     (rwspin_read,       rwspinlock_t, rwspinlock_acquire_read,       rwspinlock_release_read)
DEFINE_GUARD     (rwspin_write,      rwspinlock_t, rwspinlock_acquire_write,      rwspinlock_release_write)
DEFINE_GUARD_SAFE(rwspin_read_safe,  rwspinlock_t, rwspinlock_acquire_read_safe,  rwspinlock_release_read_safe)
DEFINE_GUARD_SAFE(rwspin_write_safe, rwspinlock_t, rwspinlock_acquire_write_safe, rwspinlock_release_write_safe)

DEFINE_GUARD     (percpu_rwspin_read,       percpu_rwspinlock_t, percpu_rwspinlock_acquire_read,       percpu_rwspinlock_release_read)
DEFINE_GUARD     (percpu_rwspin_write,      percpu_rwspinlock_t, percpu_rwspinlock_acquire_write,      percpu_rwspinlock_release_write)
DEFINE_GUARD_SAFE(percpu_rwspin_read_safe,  percpu_rwspinlock_t, percpu_rwspinlock_acquire_read_safe,  percpu_rwspinlock_release_read_safe)
DEFINE_GUARD_SAFE(percpu_rwspin_write_safe, percpu_rwspinlock_t, percpu_rwspinlock_acquire_write_safe, percpu_rwspinlock_release_write_safe)
#endif

#ifdef __cplusplus
}
#endif

#endif
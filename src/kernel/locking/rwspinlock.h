/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef KERNEL_LOCKING_RWSPINLOCK_H
#define KERNEL_LOCKING_RWSPINLOCK_H

#include <kernel/smp/cpu_limits.h>

#include <hal/align.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t state;
} rwspinlock_t;

#define RWSPINLOCK_WRITER_ACTIVE  0x80000000u
#define RWSPINLOCK_WRITER_PENDING 0x40000000u
#define RWSPINLOCK_READER_MASK    0x3FFFFFFFu

typedef struct {
    volatile uint32_t writer_seq;

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

#ifdef __cplusplus
}
#endif

#endif

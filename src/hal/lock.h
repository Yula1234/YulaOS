// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_LOCK_H
#define HAL_LOCK_H

#include <lib/dlist.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

static inline void spinlock_init(spinlock_t* lock) {
    lock->locked = 0;
}

static inline void spinlock_acquire(spinlock_t* lock) {
    uint32_t backoff = 1;

    for (;;) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0u) {
            for (uint32_t i = 0; i < backoff; i++) {
                __asm__ volatile("pause" ::: "memory");
            }

            if (backoff < 1024u) {
                backoff <<= 1;
            }
        }

        if (__atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) == 0u) {
            return;
        }

        __asm__ volatile("pause" ::: "memory");
    }
}

static inline uint32_t spinlock_acquire_safe(spinlock_t* lock) {
    uint32_t flags;
    
    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        : 
        : "memory"
    );

    uint32_t backoff = 1;

    for (;;) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0u) {
            for (uint32_t i = 0; i < backoff; i++) {
                __asm__ volatile("pause" ::: "memory");
            }

            if (backoff < 1024u) {
                backoff <<= 1;
            }
        }

        if (__atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) == 0u) {
            break;
        }

        __asm__ volatile("pause" ::: "memory");
    }
    
    return flags;
}

static inline void spinlock_release_safe(spinlock_t* lock, uint32_t flags) {
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
    
    if (flags & 0x200) {
        __asm__ volatile("sti");
    }
}

static inline int spinlock_try_acquire(spinlock_t* lock) {
    return __atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) == 0u;
}

static inline void spinlock_release(spinlock_t* lock) {
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
}

struct task;

typedef struct {
    volatile int count;

    spinlock_t lock;
    
    dlist_head_t wait_list;
} semaphore_t;

void sem_init(semaphore_t* sem, int init_count);
void sem_wait(semaphore_t* sem);
void sem_signal(semaphore_t* sem);
void sem_signal_all(semaphore_t* sem);
int sem_try_acquire(semaphore_t* sem);

typedef struct {
    semaphore_t lock;
    semaphore_t write_sem;
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

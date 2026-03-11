// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_LOCK_H
#define HAL_LOCK_H

#include <lib/dlist.h>

#include <hal/align.h>

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
void sem_reset(semaphore_t* sem, int value);
void sem_wait(semaphore_t* sem);
void sem_signal(semaphore_t* sem);
void sem_signal_all(semaphore_t* sem);
int sem_try_acquire(semaphore_t* sem);

typedef struct {
    semaphore_t sem;
} mutex_t;

static inline void mutex_init(mutex_t* m) {
    sem_init(&m->sem, 1);
}

static inline void mutex_lock(mutex_t* m) {
    sem_wait(&m->sem);
}

static inline void mutex_unlock(mutex_t* m) {
    sem_signal(&m->sem);
}

static inline int mutex_try_lock(mutex_t* m) {
    return sem_try_acquire(&m->sem);
}

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

typedef struct {
    volatile uint32_t state;
} rwspinlock_t;

static inline void rwspinlock_init(rwspinlock_t* rw) {
    rw->state = 0u;
}

static inline void rwspinlock_acquire_read(rwspinlock_t* rw) {
    uint32_t backoff = 1;

    for (;;) {
        uint32_t s = __atomic_load_n(&rw->state, __ATOMIC_ACQUIRE);
        if ((s & 0x80000000u) != 0u) {
            for (uint32_t i = 0; i < backoff; i++) {
                __asm__ volatile("pause" ::: "memory");
            }

            if (backoff < 1024u) {
                backoff <<= 1;
            }

            continue;
        }

        if ((s & 0x7FFFFFFFu) == 0x7FFFFFFFu) {
            __asm__ volatile("pause" ::: "memory");
            continue;
        }

        if (__atomic_compare_exchange_n(
                &rw->state,
                &s,
                s + 1u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            return;
        }
    }
}

static inline void rwspinlock_release_read(rwspinlock_t* rw) {
    __atomic_fetch_sub(&rw->state, 1u, __ATOMIC_RELEASE);
}

static inline void rwspinlock_acquire_write(rwspinlock_t* rw) {
    uint32_t backoff = 1;

    for (;;) {
        uint32_t expected = 0u;

        if (__atomic_compare_exchange_n(
                &rw->state,
                &expected,
                0x80000000u,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            return;
        }

        for (uint32_t i = 0; i < backoff; i++) {
            __asm__ volatile("pause" ::: "memory");
        }

        if (backoff < 1024u) {
            backoff <<= 1;
        }
    }
}

static inline void rwspinlock_release_write(rwspinlock_t* rw) {
    __atomic_store_n(&rw->state, 0u, __ATOMIC_RELEASE);
}

static inline uint32_t rwspinlock_acquire_read_safe(rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    rwspinlock_acquire_read(rw);

    return flags;
}

static inline void rwspinlock_release_read_safe(rwspinlock_t* rw, uint32_t flags) {
    rwspinlock_release_read(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

static inline uint32_t rwspinlock_acquire_write_safe(rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    rwspinlock_acquire_write(rw);

    return flags;
}

static inline void rwspinlock_release_write_safe(rwspinlock_t* rw, uint32_t flags) {
    rwspinlock_release_write(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

#ifdef __cplusplus
}
#endif


#endif

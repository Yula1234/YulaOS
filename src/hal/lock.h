// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_LOCK_H
#define HAL_LOCK_H

#include <lib/dlist.h>

#include <hal/align.h>
#include <hal/cpu.h>

#include <stdint.h>

#include <kernel/cpu_limits.h>
#include <kernel/panic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t next;
    volatile uint32_t locked;
} spinlock_qnode_t;

#define SPINLOCK_QNODE_DEPTH 128

extern spinlock_qnode_t g_spinlock_qnodes[MAX_CPUS][SPINLOCK_QNODE_DEPTH];
extern volatile uint32_t g_spinlock_qnode_tops[MAX_CPUS];

typedef struct {
    volatile uint32_t tail;
} spinlock_t;

static inline void spinlock_init(spinlock_t* lock) {
    lock->tail = 0u;
}

static inline void spinlock_qnode_cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static inline spinlock_qnode_t* spinlock_qnode_alloc(void) {
    const int cpu = hal_cpu_index();

    uint32_t idx = __atomic_fetch_add(&g_spinlock_qnode_tops[cpu], 1u, __ATOMIC_RELAXED);
    if (idx >= SPINLOCK_QNODE_DEPTH) {
        kernel_panic("spinlock qnode depth overflow", "lock.h", __LINE__, 0);
    }

    return &g_spinlock_qnodes[cpu][idx];
}

static inline spinlock_qnode_t* spinlock_qnode_current(void) {
    const int cpu = hal_cpu_index();
    uint32_t idx = __atomic_load_n(&g_spinlock_qnode_tops[cpu], __ATOMIC_RELAXED);

    if (idx == 0u) {
        kernel_panic("spinlock qnode underflow", "lock.h", __LINE__, 0);
    }

    return &g_spinlock_qnodes[cpu][idx - 1u];
}

static inline void spinlock_qnode_free(void) {
    const int cpu = hal_cpu_index();
    (void)__atomic_fetch_sub(&g_spinlock_qnode_tops[cpu], 1u, __ATOMIC_RELAXED);
}

static inline void spinlock_acquire(spinlock_t* lock) {
    spinlock_qnode_t* node = spinlock_qnode_alloc();

    __atomic_store_n(&node->next, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&node->locked, 1u, __ATOMIC_RELAXED);

    const uint32_t me = (uint32_t)(uintptr_t)node;

    uint32_t prev = __atomic_exchange_n(&lock->tail, me, __ATOMIC_ACQ_REL);
    if (prev == 0u) {
        __atomic_store_n(&node->locked, 0u, __ATOMIC_RELAXED);
        return;
    }

    spinlock_qnode_t* pred = (spinlock_qnode_t*)(uintptr_t)prev;
    __atomic_store_n(&pred->next, me, __ATOMIC_RELEASE);

    while (__atomic_load_n(&node->locked, __ATOMIC_ACQUIRE) != 0u) {
        spinlock_qnode_cpu_relax();
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

    spinlock_acquire(lock);

    return flags;
}

static inline int spinlock_try_acquire(spinlock_t* lock) {
    spinlock_qnode_t* node = spinlock_qnode_alloc();

    __atomic_store_n(&node->next, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&node->locked, 0u, __ATOMIC_RELAXED);

    const uint32_t me = (uint32_t)(uintptr_t)node;

    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(
            &lock->tail,
            &expected,
            me,
            0,
            __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED
        )) {
        return 1;
    }

    spinlock_qnode_free();
    return 0;
}

static inline void spinlock_release(spinlock_t* lock) {
    spinlock_qnode_t* node = spinlock_qnode_current();
    const uint32_t me = (uint32_t)(uintptr_t)node;

    uint32_t succ = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE);
    if (succ == 0u) {
        uint32_t expected = me;
        if (__atomic_compare_exchange_n(
                &lock->tail,
                &expected,
                0u,
                0,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED
            )) {
            spinlock_qnode_free();
            return;
        }

        do {
            spinlock_qnode_cpu_relax();
            succ = __atomic_load_n(&node->next, __ATOMIC_ACQUIRE);
        } while (succ == 0u);
    }

    spinlock_qnode_t* next = (spinlock_qnode_t*)(uintptr_t)succ;
    __atomic_store_n(&next->locked, 0u, __ATOMIC_RELEASE);

    spinlock_qnode_free();
}

static inline void spinlock_release_safe(spinlock_t* lock, uint32_t flags) {
    spinlock_release(lock);

    if (flags & 0x200) {
        __asm__ volatile("sti");
    }
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
int sem_wait_timeout(semaphore_t* sem, uint32_t deadline_tick);
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
    semaphore_t turnstile;
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

static inline void rwspinlock_init(rwspinlock_t* rw) {
    rw->state = 0u;
}

static inline void rwspinlock_acquire_read(rwspinlock_t* rw) {
    uint32_t backoff = 1;

    for (;;) {
        uint32_t s = __atomic_load_n(&rw->state, __ATOMIC_ACQUIRE);
        if ((s & (RWSPINLOCK_WRITER_ACTIVE | RWSPINLOCK_WRITER_PENDING)) != 0u) {
            for (uint32_t i = 0; i < backoff; i++) {
                __asm__ volatile("pause" ::: "memory");
            }

            if (backoff < 1024u) {
                backoff <<= 1;
            }

            continue;
        }

        if ((s & RWSPINLOCK_READER_MASK) == RWSPINLOCK_READER_MASK) {
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

    __atomic_fetch_or(&rw->state, RWSPINLOCK_WRITER_PENDING, __ATOMIC_RELAXED);

    for (;;) {
        uint32_t expected = RWSPINLOCK_WRITER_PENDING;

        if (__atomic_compare_exchange_n(
                &rw->state,
                &expected,
                RWSPINLOCK_WRITER_ACTIVE,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            return;
        }

        __atomic_fetch_or(&rw->state, RWSPINLOCK_WRITER_PENDING, __ATOMIC_RELAXED);

        for (uint32_t i = 0; i < backoff; i++) {
            __asm__ volatile("pause" ::: "memory");
        }

        if (backoff < 1024u) {
            backoff <<= 1;
        }
    }
}

static inline void rwspinlock_release_write(rwspinlock_t* rw) {
    __atomic_fetch_and(&rw->state, ~RWSPINLOCK_WRITER_ACTIVE, __ATOMIC_RELEASE);
}

static inline void percpu_rwspinlock_init(percpu_rwspinlock_t* rw) {
    rw->writer_seq = 0u;

    for (int i = 0; i < MAX_CPUS; i++) {
        __atomic_store_n(&rw->readers[i].count, 0u, __ATOMIC_RELAXED);
    }
}

static inline void percpu_rwspinlock_acquire_read(percpu_rwspinlock_t* rw) {
    const int cpu = hal_cpu_index();
    uint32_t backoff = 1;

    for (;;) {
        uint32_t seq = __atomic_load_n(&rw->writer_seq, __ATOMIC_ACQUIRE);
        if ((seq & 1u) != 0u) {
            for (uint32_t i = 0; i < backoff; i++) {
                __asm__ volatile("pause" ::: "memory");
            }

            if (backoff < 1024u) {
                backoff <<= 1;
            }

            continue;
        }

        __atomic_fetch_add(&rw->readers[cpu].count, 1u, __ATOMIC_ACQ_REL);

        uint32_t seq2 = __atomic_load_n(&rw->writer_seq, __ATOMIC_ACQUIRE);
        if (seq2 == seq && (seq2 & 1u) == 0u) {
            return;
        }

        __atomic_fetch_sub(&rw->readers[cpu].count, 1u, __ATOMIC_RELEASE);
    }
}

static inline void percpu_rwspinlock_release_read(percpu_rwspinlock_t* rw) {
    const int cpu = hal_cpu_index();
    __atomic_fetch_sub(&rw->readers[cpu].count, 1u, __ATOMIC_RELEASE);
}

static inline void percpu_rwspinlock_acquire_write(percpu_rwspinlock_t* rw) {
    uint32_t backoff = 1;

    for (;;) {
        uint32_t seq = __atomic_load_n(&rw->writer_seq, __ATOMIC_ACQUIRE);
        if ((seq & 1u) != 0u) {
            for (uint32_t i = 0; i < backoff; i++) {
                __asm__ volatile("pause" ::: "memory");
            }

            if (backoff < 1024u) {
                backoff <<= 1;
            }

            continue;
        }

        uint32_t desired = seq + 1u;
        if (__atomic_compare_exchange_n(
                &rw->writer_seq,
                &seq,
                desired,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            break;
        }
    }

    for (;;) {
        int any = 0;

        for (int i = 0; i < MAX_CPUS; i++) {
            if (__atomic_load_n(&rw->readers[i].count, __ATOMIC_ACQUIRE) != 0u) {
                any = 1;
                break;
            }
        }

        if (!any) {
            return;
        }

        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void percpu_rwspinlock_release_write(percpu_rwspinlock_t* rw) {
    __atomic_fetch_add(&rw->writer_seq, 1u, __ATOMIC_RELEASE);
}

static inline uint32_t percpu_rwspinlock_acquire_read_safe(percpu_rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    percpu_rwspinlock_acquire_read(rw);

    return flags;
}

static inline void percpu_rwspinlock_release_read_safe(percpu_rwspinlock_t* rw, uint32_t flags) {
    percpu_rwspinlock_release_read(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
}

static inline uint32_t percpu_rwspinlock_acquire_write_safe(percpu_rwspinlock_t* rw) {
    uint32_t flags;

    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );

    percpu_rwspinlock_acquire_write(rw);

    return flags;
}

static inline void percpu_rwspinlock_release_write_safe(percpu_rwspinlock_t* rw, uint32_t flags) {
    percpu_rwspinlock_release_write(rw);

    if (flags & 0x200u) {
        __asm__ volatile("sti");
    }
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

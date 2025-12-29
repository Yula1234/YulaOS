#ifndef HAL_LOCK_H
#define HAL_LOCK_H

#include <lib/dlist.h>

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

static inline void spinlock_init(spinlock_t* lock) {
    lock->locked = 0;
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

    while (1) {
        if (__sync_lock_test_and_set(&lock->locked, 1) == 0) {
            break;
        }
        __asm__ volatile ("pause");
    }
    
    __sync_synchronize();
    
    return flags;
}

static inline void spinlock_release_safe(spinlock_t* lock, uint32_t flags) {
    __sync_synchronize();
    
    __sync_lock_release(&lock->locked);
    
    if (flags & 0x200) {
        __asm__ volatile("sti");
    }
}

static inline int spinlock_try_acquire(spinlock_t* lock) {
    return __sync_lock_test_and_set(&lock->locked, 1) == 0;
}

static inline void spinlock_release(spinlock_t* lock) {
    __sync_lock_release(&lock->locked);
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


#endif
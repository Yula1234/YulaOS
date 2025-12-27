#ifndef HAL_LOCK_H
#define HAL_LOCK_H

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

#endif
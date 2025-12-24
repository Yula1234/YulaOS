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
        : // no inputs
        : "memory"
    );
    
    lock->locked = 1;
    
    return flags;
}

static inline void spinlock_release_safe(spinlock_t* lock, uint32_t flags) {
    lock->locked = 0;
    
    if (flags & 0x200) {
        __asm__ volatile("sti");
    }
}

#endif
#include <kernel/locking/guards.h>

#include <hal/lock.h>

#include <stdint.h>

#include "irq.h"

static uint32_t g_irq_vector_bitmap[8] = {0};
static spinlock_t g_irq_vector_lock;

extern irq_handler_t irq_vector_handlers[256];

int irq_alloc_vector(void) {
    guard_spinlock_safe(&g_irq_vector_lock);

    for (int vec = 48; vec < 240; vec++) {
        if (vec == 0x80) {
            continue;
        }
        
        int idx = vec / 32;
        int bit = vec % 32;

        if ((g_irq_vector_bitmap[idx] & (1u << bit)) == 0) {
            g_irq_vector_bitmap[idx] |= (1u << bit);

            return vec;
        }
    }

    return -1;
}

void irq_free_vector(int vector) {
    if (vector < 48
        || vector >= 240
        || vector == 0x80) {

        return;
    }

    guard_spinlock_safe(&g_irq_vector_lock);
    
    int idx = vector / 32;
    int bit = vector % 32;
    g_irq_vector_bitmap[idx] &= ~(1u << bit);
    
    irq_vector_handlers[vector] = 0;
}
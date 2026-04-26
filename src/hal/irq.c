#include <kernel/locking/spinlock.h>

#include <stdint.h>

#include "irq.h"

static uint32_t g_irq_vector_bitmap[8] = {0};
static spinlock_t g_irq_vector_lock;

extern irq_handler_t irq_vector_handlers[256];

int irq_alloc_vector(void) {
    guard(spinlock_safe)(&g_irq_vector_lock);

    for (int i = 1; i < 8; i++) {
        uint32_t free_bits = ~g_irq_vector_bitmap[i];

        if (i == 1) free_bits &= ~0x0000FFFFu;
        if (i == 4) free_bits &= ~0x00000001u;
        if (i == 7) free_bits &= 0x0000FFFFu;

        if (free_bits != 0) {
            int bit = __builtin_ctz(free_bits);

            g_irq_vector_bitmap[i] |= (1u << bit);
            
            return (i * 32) + bit;
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

    guard(spinlock_safe)(&g_irq_vector_lock);
    
    int idx = vector / 32;
    int bit = vector % 32;
    g_irq_vector_bitmap[idx] &= ~(1u << bit);
    
    irq_vector_handlers[vector] = 0;
}
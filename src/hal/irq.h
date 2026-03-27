#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <arch/i386/idt.h>

typedef void (*irq_handler_t)(registers_t* regs, void* ctx);

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t get_eflags(void) {
    uint32_t eflags = 0u;

    __asm__ volatile("pushfl; popl %0" : "=r"(eflags) : : "memory");

    return eflags;
}

void irq_install_handler(int irq_no, irq_handler_t handler, void* ctx);

void irq_install_vector_handler(int vector, irq_handler_t handler, void* ctx);

void irq_set_legacy_pic_enabled(int enabled);

int irq_alloc_vector(void);

void irq_free_vector(int vector);

#ifdef __cplusplus
}
#endif

#endif
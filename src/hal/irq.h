// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234
    
#ifndef HAL_IRQ_H
#define HAL_IRQ_H

#include <arch/i386/idt.h>

typedef void (*irq_handler_t)(registers_t*);

void irq_install_handler(int irq_no, irq_handler_t handler);

void irq_install_vector_handler(int vector, irq_handler_t handler);

#endif
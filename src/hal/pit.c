// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "pit.h"
#include "io.h"

void pit_init(uint32_t frequency) {
    uint32_t divisor = 1193182 / frequency;
    outb(0x43, 0x36);    
    outb(0x40, (uint8_t)(divisor & 0xFF));        
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_SIMD_H
#define HAL_SIMD_H

#include <stdint.h>

static inline void kernel_init_simd(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);
    cr0 |= (1 << 1); 
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);
    cr4 |= (1 << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

static inline void fpu_save(uint8_t* buffer) {
    __asm__ volatile("fxsave (%0)" :: "r"(buffer) : "memory");
}

static inline void fpu_restore(uint8_t* buffer) {
    __asm__ volatile("fxrstor (%0)" :: "r"(buffer) : "memory");
}

#endif
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

// Implementation of __udivdi3 for 64-bit unsigned division
// Required for freestanding environment where libgcc is not available

#include <stdint.h>

uint64_t __udivdi3(uint64_t num, uint64_t den) {
    if (den == 0) {
        return 0;
    }
    
    if (num == 0) {
        return 0;
    }
    
    if (num < den) {
        return 0;
    }
    
    if (num == den) {
        return 1;
    }
    
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    
    for (int i = 63; i >= 0; i--) {
        remainder <<= 1;
        remainder |= (num >> i) & 1;
        
        if (remainder >= den) {
            remainder -= den;
            quotient |= (1ULL << i);
        }
    }
    
    return quotient;
}


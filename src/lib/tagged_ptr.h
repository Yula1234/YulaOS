/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef _LIB_TAGGED_PTR_H
#define _LIB_TAGGED_PTR_H

#include <lib/compiler.h>

/*
 * tagged_ptr_t - A 64-bit pointer with a version tag for 32-bit x86.
 * 
 * Used for building ABA-safe lock-free data structures.
 * The structure is 8-byte aligned to allow atomic 64-bit operations
 * using the cmpxchg8b instruction.
 */
typedef struct {
    void*    ptr_;
    uint32_t version_;
} __attribute__((aligned(8))) tagged_ptr_t;

___inline int tagged_ptr_cas(
    volatile tagged_ptr_t* dst,
    tagged_ptr_t           expected,
    tagged_ptr_t           desired
) {
    uint8_t success;
    
    __asm__ volatile(
        "lock cmpxchg8b (%[ptr]) \n\t"
        "sete %[ok]              \n\t"
        : "+A"  (*(uint64_t*)&expected),
          [ok]  "=qm" (success)
        : [ptr] "S"   (dst),
          "b"   (desired.ptr_),
          "c"   (desired.version_)
        : "memory", "cc"
    );
    
    return success;
}

___inline tagged_ptr_t tagged_ptr_load(volatile tagged_ptr_t* src) {
    tagged_ptr_t result;
    uint32_t zero = 0;
    
    __asm__ volatile(
        "lock cmpxchg8b (%[ptr]) \n\t"
        : "=A" (*(uint64_t*)&result)
        : [ptr] "S" (src),
          "a" (zero), "d" (zero),
          "b" (zero), "c" (zero)
        : "memory", "cc"
    );
    
    return result;
}

#endif
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef HAL_CPU_H
#define HAL_CPU_H

#include <kernel/smp/cpu.h>

#include <lib/compiler.h>

#include "cpu.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

___inline int hal_cpu_index(void) {
    cpu_t* cpu = cpu_current();

    const int idx = cpu ? cpu->index : 0;

#ifdef __cplusplus
    if (kernel::unlikely(idx < 0 || idx >= MAX_CPUS)) {
#else
    if (unlikely(idx < 0 || idx >= MAX_CPUS)) {
#endif
        return 0;
    }

    return idx;
}


#define cpu_relax(...) __asm__ volatile("pause" ::: "memory")

___inline uint32_t this_cpu_inc(uint32_t* var) {
    uint32_t val = 1;

    __asm__ volatile("xaddl %0, %1" : "+r" (val), "+m" (*var) :: "memory");

    return val;
}

___inline void this_cpu_dec(uint32_t* var) {
    __asm__ volatile("decl %0" : "+m" (*var) :: "memory");
}

#ifdef __cplusplus
}
#endif

#endif

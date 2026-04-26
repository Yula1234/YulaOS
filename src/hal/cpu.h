/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef HAL_CPU_H
#define HAL_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

int hal_cpu_index(void);

#define cpu_relax() __asm__ volatile("pause" ::: "memory")

__attribute__((always_inline)) static inline uint32_t this_cpu_inc(uint32_t* var) {
    const uint32_t val = 1;

    __asm__ volatile("xaddl %0, %1" : "+r" (val), "+m" (*var) :: "memory");

    return val;
}

__attribute__((always_inline)) static inline void this_cpu_dec(uint32_t* var) {
    __asm__ volatile("decl %0" : "+m" (*var) :: "memory");
}

#ifdef __cplusplus
}
#endif

#endif

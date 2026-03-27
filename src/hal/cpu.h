// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_CPU_H
#define HAL_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

int hal_cpu_index(void);

__attribute__((always_inline)) static inline void cpu_relax(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

#ifdef __cplusplus
}
#endif

#endif

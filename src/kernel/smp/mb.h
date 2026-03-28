/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef HAL_MB_H
#define HAL_MB_H

#define compiler_mb() __asm__ volatile("" ::: "memory")

#define smp_mb()  __asm__ volatile("mfence" ::: "memory")

#define smp_rmb() __asm__ volatile("lfence" ::: "memory")

#define smp_wmb() __asm__ volatile("sfence" ::: "memory")

#endif // HAL_MB_H
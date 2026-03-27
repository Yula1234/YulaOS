// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_ALIGN_H
#define HAL_ALIGN_H

#ifndef HAL_CACHELINE_SIZE
#define HAL_CACHELINE_SIZE 64u
#endif

#define __cacheline_aligned __attribute__((aligned(HAL_CACHELINE_SIZE)))

#ifndef __cplusplus

#include <lib/compiler.h>

#include <stddef.h>

static inline size_t align_up(size_t v, size_t a) {
    if (unlikely(a == 0u)) {
        return v;
    }

    return (v + a - 1u) & ~(a - 1u);
}

#endif

#endif

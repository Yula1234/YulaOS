/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rcu_ptr {
    void* ptr;
} rcu_ptr_t;

typedef struct rcu_head {
    struct rcu_head* next;
    void (*func)(struct rcu_head*);
} rcu_head_t;

#ifdef __cplusplus
}
#endif

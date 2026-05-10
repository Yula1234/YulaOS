/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_IDR_H
#define LIB_IDR_H

#include <kernel/locking/spinlock.h>

#include <lib/radixtree.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct idr {
    radix_tree_t tree;
    spinlock_t   lock;

    uint32_t     next_id;
} idr_t;

void idr_init(idr_t* idr);
void idr_destroy(idr_t* idr);

int idr_alloc(idr_t* idr, void* ptr);
void* idr_find(idr_t* idr, int id);
void idr_remove(idr_t* idr, int id);

#ifdef __cplusplus
}
#endif

#endif
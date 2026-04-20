/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_CHASHMAP_H
#define LIB_CHASHMAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chashmap;
typedef struct chashmap chashmap_t;

typedef void (*chashmap_cb_t)(
    uint32_t key,
    void* value,
    void* ctx
);

chashmap_t* chashmap_create(void);

void chashmap_destroy(chashmap_t* cmap);

int chashmap_insert_unique(chashmap_t* cmap, uint32_t key, void* value);

int chashmap_set(chashmap_t* cmap, uint32_t key, void* value);

void* chashmap_find(chashmap_t* cmap, uint32_t key);

void* chashmap_remove_and_get(chashmap_t* cmap, uint32_t key);

void chashmap_remove(chashmap_t* cmap, uint32_t key);

void chashmap_clear(chashmap_t* cmap);

void chashmap_iterate(chashmap_t* cmap, chashmap_cb_t cb, void* ctx);

#ifdef __cplusplus
}
#endif

#endif
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef MM_SHRINKER_H
#define MM_SHRINKER_H

#include <lib/dlist.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef size_t (*shrinker_cb_t)(size_t target_pages, void* ctx);

typedef struct shrinker {
    const char* name;
    shrinker_cb_t reclaim;
    void* ctx;
    
    dlist_head_t list;
} shrinker_t;

void shrinker_init(void);
void shrinker_start_kthread(void);

void register_shrinker(shrinker_t* s);
void unregister_shrinker(shrinker_t* s);

#ifdef __cplusplus
}
#endif

#endif
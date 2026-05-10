/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/compiler.h>
#include <lib/string.h>
#include <lib/idr.h>

#include <mm/heap.h>

#include <stdint.h>
#include <stddef.h>

void idr_init(idr_t* idr) {
    if (unlikely(!idr))
        return;

    memset(idr, 0, sizeof(*idr));

    radix_tree_init(&idr->tree);
    spinlock_init(&idr->lock);

    idr->next_id = 1u;
}

void idr_destroy(idr_t* idr) {
    if (unlikely(!idr))
        return;

    radix_tree_destroy(&idr->tree);
}

int idr_alloc(idr_t* idr, void* ptr) {
    if (unlikely(!idr
        || !ptr))
        return -1;

    guard(spinlock_safe)(&idr->lock);

    const uint32_t start_id = idr->next_id;
    uint32_t id = start_id;

    for (;;) {
        void* existing = radix_tree_lookup(&idr->tree, id);

        if (existing == NULL) {
            const int rc = radix_tree_insert(&idr->tree, id, ptr);

            if (likely(rc == 0)) {
                uint32_t next = id + 1u;

                if (unlikely(next == 0u))
                    next = 1u;

                idr->next_id = next;

                return (int)id;
            }

            /*
             * radix_tree_insert fails only on OOM or collision.
             * Collision is impossible here, so we hit OOM.
             */
            return -1;
        }

        id++;

        if (unlikely(id == 0u))
            id = 1u;

        if (unlikely(id == start_id))
            break;
    }

    return -1;
}

void* idr_find(idr_t* idr, int id) {
    if (unlikely(!idr
        || id <= 0))
        return NULL;

    return radix_tree_lookup(&idr->tree, (uint32_t)id);
}

void idr_remove(idr_t* idr, int id) {
    if (unlikely(!idr
        || id <= 0))
        return;

    (void)radix_tree_remove(&idr->tree, (uint32_t)id);
}
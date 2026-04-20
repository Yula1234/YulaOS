/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/compiler.h>
#include <lib/hash_map.h>
#include <lib/cpp/new.h>

#include <mm/heap.h>

#include <stdint.h>
#include <stddef.h>

#include "chashmap.h"

struct chashmap {
    HashMap<uint32_t, void*, 64> map_;
};

extern "C" chashmap_t* chashmap_create(void) {
    void* mem = kmalloc(sizeof(chashmap_t));

    if (kernel::unlikely(!mem)) {
        return nullptr;
    }

    chashmap_t* cmap = new (mem) chashmap_t();

    return cmap;
}

extern "C" void chashmap_destroy(chashmap_t* cmap) {
    if (kernel::unlikely(!cmap)) {
        return;
    }

    cmap->~chashmap_t();

    kfree(cmap);
}

extern "C" int chashmap_insert_unique(chashmap_t* cmap, uint32_t key, void* value) {
    if (kernel::unlikely(!cmap || !value)) {
        return -1;
    }

    const auto res = cmap->map_.insert_unique_ex(key, value);

    if (kernel::unlikely(res != decltype(cmap->map_)::InsertUniqueResult::Inserted)) {
        return -1;
    }

    return 0;
}

extern "C" int chashmap_set(chashmap_t* cmap, uint32_t key, void* value) {
    if (kernel::unlikely(!cmap || !value)) {
        return -1;
    }

    const auto res = cmap->map_.insert_or_assign_ex(key, value);

    if (kernel::unlikely(res == decltype(cmap->map_)::InsertOrAssignResult::Failed
        || res == decltype(cmap->map_)::InsertOrAssignResult::OutOfMemory)) {
        return -1;
    }

    return 0;
}

extern "C" void* chashmap_find(chashmap_t* cmap, uint32_t key) {
    if (kernel::unlikely(!cmap)) {
        return nullptr;
    }

    void* value = nullptr;

    const bool found = cmap->map_.try_get(key, value);

    if (kernel::unlikely(!found)) {
        return nullptr;
    }

    return value;
}

extern "C" void* chashmap_remove_and_get(chashmap_t* cmap, uint32_t key) {
    if (kernel::unlikely(!cmap)) {
        return nullptr;
    }

    void* value = nullptr;

    const bool removed = cmap->map_.remove_and_get(key, value);

    if (kernel::unlikely(!removed)) {
        return nullptr;
    }

    return value;
}

extern "C" void chashmap_remove(chashmap_t* cmap, uint32_t key) {
    (void)chashmap_remove_and_get(cmap, key);
}

extern "C" void chashmap_clear(chashmap_t* cmap) {
    if (kernel::unlikely(!cmap)) {
        return;
    }

    cmap->map_.clear();
}

extern "C" void chashmap_iterate(chashmap_t* cmap, chashmap_cb_t cb, void* ctx) {
    if (kernel::unlikely(!cmap || !cb)) {
        return;
    }

    auto view = cmap->map_.locked_view();

    for (auto it = view.begin(); it != view.end(); ++it) {
        auto kv = *it;

        cb(kv.first, kv.second, ctx);
    }
}
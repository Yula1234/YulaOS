/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/string.h>
#include <lib/dlist.h>

#include <hal/lock.h>

#include <mm/heap.h>

#include "api.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096u
#endif

typedef struct {
    dlist_head_t node;

    void* vaddr;
    uint32_t phys;
} dma_pool_page_t;

typedef struct {
    void* next;
    uint32_t phys;
} dma_pool_slot_hdr_t;

struct dma_pool {
    char* name;

    size_t obj_size;
    size_t align;

    size_t obj_off;
    size_t slot_size;

    spinlock_t lock;

    void* free_list;
    dlist_head_t pages;
};

static size_t align_up(size_t v, size_t a) {
    if (a == 0u) {
        return v;
    }

    return (v + a - 1u) & ~(a - 1u);
}

static int is_pow2(size_t v) {
    return v && ((v & (v - 1u)) == 0u);
}

static char* dup_name(const char* s) {
    if (!s) {
        return 0;
    }

    const size_t len = strlen(s);

    char* out = (char*)kmalloc(len + 1u);
    if (!out) {
        return 0;
    }

    memcpy(out, s, len);
    out[len] = 0;

    return out;
}

static int dma_pool_prepare_page(
    dma_pool_t* pool,
    dma_pool_page_t** out_page,
    dma_pool_slot_hdr_t** out_head,
    dma_pool_slot_hdr_t** out_tail
) {
    if (!pool || !out_page || !out_head || !out_tail) {
        return 0;
    }

    *out_page = 0;
    *out_head = 0;
    *out_tail = 0;

    uint32_t page_phys = 0;
    void* page = dma_alloc_coherent(PAGE_SIZE, &page_phys);
    if (!page || !page_phys) {
        return 0;
    }

    if ((page_phys & (uint32_t)(pool->align - 1u)) != 0u) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        return 0;
    }

    dma_pool_page_t* rec = (dma_pool_page_t*)kzalloc(sizeof(*rec));
    if (!rec) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        return 0;
    }

    dlist_init(&rec->node);
    rec->vaddr = page;
    rec->phys = page_phys;

    const size_t count = PAGE_SIZE / pool->slot_size;

    dma_pool_slot_hdr_t* head = 0;
    dma_pool_slot_hdr_t* tail = 0;

    for (size_t i = 0; i < count; i++) {
        uint8_t* slot = (uint8_t*)page + (i * pool->slot_size);
        dma_pool_slot_hdr_t* hdr = (dma_pool_slot_hdr_t*)slot;

        hdr->phys = page_phys + (uint32_t)(i * pool->slot_size) + (uint32_t)pool->obj_off;
        hdr->next = 0;

        if (!head) {
            head = hdr;
            tail = hdr;
        } else {
            tail->next = hdr;
            tail = hdr;
        }
    }

    *out_page = rec;
    *out_head = head;
    *out_tail = tail;

    return head != 0;
}

dma_pool_t* dma_pool_create(const char* name, size_t obj_size, size_t align) {
    if (!name || obj_size == 0u) {
        return 0;
    }

    if (align == 0u) {
        align = sizeof(void*);
    }

    if (!is_pow2(align) || align > PAGE_SIZE) {
        return 0;
    }

    dma_pool_t* pool = (dma_pool_t*)kzalloc(sizeof(*pool));
    if (!pool) {
        return 0;
    }

    pool->name = dup_name(name);
    if (!pool->name) {
        kfree(pool);
        return 0;
    }

    pool->obj_size = obj_size;
    pool->align = align;

    pool->obj_off = align_up(sizeof(dma_pool_slot_hdr_t), align);
    pool->slot_size = align_up(pool->obj_off + obj_size, align);

    if (pool->slot_size == 0u || pool->slot_size > PAGE_SIZE) {
        kfree(pool->name);
        kfree(pool);
        return 0;
    }

    spinlock_init(&pool->lock);

    pool->free_list = 0;
    dlist_init(&pool->pages);

    return pool;
}

void* dma_pool_alloc(dma_pool_t* pool, uint32_t* out_phys) {
    if (!pool || !out_phys) {
        return 0;
    }

    uint32_t flags = spinlock_acquire_safe(&pool->lock);

    if (pool->free_list) {
        dma_pool_slot_hdr_t* hdr = (dma_pool_slot_hdr_t*)pool->free_list;
        pool->free_list = hdr->next;

        spinlock_release_safe(&pool->lock, flags);

        *out_phys = hdr->phys;
        return (uint8_t*)hdr + pool->obj_off;
    }

    spinlock_release_safe(&pool->lock, flags);

    dma_pool_page_t* rec = 0;
    dma_pool_slot_hdr_t* local_head = 0;
    dma_pool_slot_hdr_t* local_tail = 0;

    if (!dma_pool_prepare_page(pool, &rec, &local_head, &local_tail)) {
        if (rec) {
            if (rec->vaddr) {
                dma_free_coherent(rec->vaddr, PAGE_SIZE, rec->phys);
            }

            kfree(rec);
        }

        return 0;
    }

    dma_pool_slot_hdr_t* mine = local_head;
    local_head = (dma_pool_slot_hdr_t*)mine->next;
    mine->next = 0;

    flags = spinlock_acquire_safe(&pool->lock);

    if (local_head) {
        local_tail->next = pool->free_list;
        pool->free_list = local_head;
    }

    dlist_add_tail(&rec->node, &pool->pages);

    spinlock_release_safe(&pool->lock, flags);

    *out_phys = mine->phys;
    return (uint8_t*)mine + pool->obj_off;
}

void dma_pool_free(dma_pool_t* pool, void* vaddr) {
    if (!pool || !vaddr) {
        return;
    }

    uint8_t* obj = (uint8_t*)vaddr;
    dma_pool_slot_hdr_t* hdr = (dma_pool_slot_hdr_t*)(obj - pool->obj_off);

    uint32_t flags = spinlock_acquire_safe(&pool->lock);

    hdr->next = pool->free_list;
    pool->free_list = hdr;

    spinlock_release_safe(&pool->lock, flags);
}

void dma_pool_destroy(dma_pool_t* pool) {
    if (!pool) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&pool->lock);

    dlist_head_t* it = pool->pages.next;
    while (it && it != &pool->pages) {
        dma_pool_page_t* p = container_of(it, dma_pool_page_t, node);
        it = it->next;

        dlist_del(&p->node);

        if (p->vaddr) {
            dma_free_coherent(p->vaddr, PAGE_SIZE, p->phys);
        }

        kfree(p);
    }

    pool->free_list = 0;

    spinlock_release_safe(&pool->lock, flags);

    if (pool->name) {
        kfree(pool->name);
        pool->name = 0;
    }

    kfree(pool);
}

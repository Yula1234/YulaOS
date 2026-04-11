/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/smp/cpu_limits.h>

#include <hal/align.h>
#include <hal/lock.h>
#include <hal/cpu.h>
#include <hal/irq.h>

#include <lib/compiler.h>
#include <lib/string.h>
#include <lib/dlist.h>

#include <mm/heap.h>

#include "api.h"

#ifndef PAGE_SIZE

#define PAGE_SIZE 4096u

#endif

#define DMA_POOL_PCP_MAX   32u
#define DMA_POOL_PCP_BATCH 16u

typedef struct DmaPoolPage {
    dlist_head_t node_;

    void* vaddr_;
    uint32_t phys_;
} DmaPoolPage;

typedef struct DmaPoolSlotHdr {
    struct DmaPoolSlotHdr* next_;

    uint32_t phys_;
} DmaPoolSlotHdr;

typedef struct PerCpuDmaCache {
    DmaPoolSlotHdr* free_list_;

    uint32_t count_;
} __cacheline_aligned PerCpuDmaCache;

struct dma_pool {
    /* hot data for per-cpu local allocation */

    uint32_t obj_off_;
    uint32_t slot_size_;

    /* slow path data */

    spinlock_t lock_;
    DmaPoolSlotHdr* global_free_;

    uint8_t         _pad0[48];
    /* end of one cacheline for more or less hot data */

    /* cacheline 2 for cold data */

    spinlock_t grow_lock_;

    dlist_head_t pages_;
    
    uint32_t obj_size_;
    uint32_t align_;
    
    char* name_;
    
    uint8_t _pad1[36];

    /* cacheline 3 */

    PerCpuDmaCache  cpu_cache_[MAX_CPUS];
} __cacheline_aligned;

static inline int is_pow2(size_t v) {
    return v && ((v & (v - 1u)) == 0u);
}

static char* dup_name(const char* s) {
    if (unlikely(!s)) {
        return 0;
    }

    const size_t len = strlen(s);

    char* out = (char*)kmalloc(len + 1u);
    if (unlikely(!out)) {
        return 0;
    }

    memcpy(out, s, len);
    out[len] = '\0';

    return out;
}

/*
 * Allocate a new backing DMA page from the system and carve it into slots.
 * This runs entirely lockless with respect to the pool's internal locks.
 */
static int dma_pool_prepare_page(
    dma_pool_t* pool, DmaPoolPage** out_page,
    DmaPoolSlotHdr** out_head, DmaPoolSlotHdr** out_tail
) {
    if (unlikely(
        !pool
        || !out_page
        || !out_head
        || !out_tail
    )) {
        return 0;
    }

    *out_page = 0;
    *out_head = 0;
    *out_tail = 0;

    uint32_t page_phys = 0;
    void* page = dma_alloc_coherent(PAGE_SIZE, &page_phys);

    if (unlikely(!page || !page_phys)) {
        return 0;
    }

    /* 
     * Hardware expects strict alignment. If the underlying allocator
     * gives us a misaligned page, we cannot satisfy the pool contract.
     */
    if (unlikely((page_phys & (uint32_t)(pool->align_ - 1u)) != 0u)) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        return 0;
    }

    DmaPoolPage* rec = (DmaPoolPage*)kzalloc(sizeof(*rec));
    if (unlikely(!rec)) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        return 0;
    }

    dlist_init(&rec->node_);
    rec->vaddr_ = page;
    rec->phys_ = page_phys;

    const size_t count = PAGE_SIZE / pool->slot_size_;

    DmaPoolSlotHdr* head = 0;
    DmaPoolSlotHdr* tail = 0;

    /*
     * Link the freshly carved slots sequentially. The hot path will graft
     * this local list onto the global free list in O(1).
     */
    for (size_t i = 0; i < count; i++) {
        uint8_t* slot_ptr = (uint8_t*)page + (i * pool->slot_size_);
        DmaPoolSlotHdr* hdr = (DmaPoolSlotHdr*)slot_ptr;

        hdr->phys_ = page_phys + (uint32_t)(i * pool->slot_size_) + (uint32_t)pool->obj_off_;
        hdr->next_ = 0;

        if (!head) {
            head = hdr;
        } else {
            tail->next_ = hdr;
        }

        tail = hdr;
    }

    *out_page = rec;
    *out_head = head;
    *out_tail = tail;

    return head != 0;
}

dma_pool_t* dma_pool_create(const char* name, size_t obj_size, size_t align) {
    if (unlikely(!name || obj_size == 0u)) {
        return 0;
    }

    if (align == 0u) {
        align = sizeof(void*);
    }

    if (unlikely(!is_pow2(align) || align > PAGE_SIZE)) {
        return 0;
    }

    dma_pool_t* pool = (dma_pool_t*)kzalloc(sizeof(*pool));
    if (unlikely(!pool)) {
        return 0;
    }

    pool->name_ = dup_name(name);
    if (unlikely(!pool->name_)) {
        kfree(pool);
        return 0;
    }

    pool->obj_size_ = obj_size;
    pool->align_ = align;

    pool->obj_off_ = align_up(sizeof(DmaPoolSlotHdr), align);
    pool->slot_size_ = align_up(pool->obj_off_ + obj_size, align);

    if (unlikely(pool->slot_size_ == 0u || pool->slot_size_ > PAGE_SIZE)) {
        kfree(pool->name_);
        kfree(pool);
        return 0;
    }

    spinlock_init(&pool->lock_);
    spinlock_init(&pool->grow_lock_);

    pool->global_free_ = 0;
    
    dlist_init(&pool->pages_);

    for (int i = 0; i < MAX_CPUS; i++) {
        pool->cpu_cache_[i].free_list_ = 0;
        pool->cpu_cache_[i].count_ = 0u;
    }

    return pool;
}

void* dma_pool_alloc(dma_pool_t* pool, uint32_t* out_phys) {
    if (unlikely(!pool || !out_phys)) {
        return 0;
    }

    uint32_t irq_flags = irq_save();

    const int cpu = hal_cpu_index();
    PerCpuDmaCache* pcp = &pool->cpu_cache_[cpu];

    /*
     * Fast path: serve directly from the per-CPU magazine.
     * IRQ disablement ensures we do not migrate to another CPU
     * and prevents reentrancy races with IRQ handlers.
     */
    if (likely(pcp->count_ > 0u)) {
        DmaPoolSlotHdr* hdr = pcp->free_list_;
        pcp->free_list_ = hdr->next_;
        pcp->count_--;

        irq_restore(irq_flags);

        *out_phys = hdr->phys_;
        return (uint8_t*)hdr + pool->obj_off_;
    }

    /*
     * Slow path: replenish the per-CPU magazine from the global pool.
     */
    spinlock_acquire(&pool->lock_);

retry_global:
    if (pool->global_free_) {
        uint32_t transferred = 0u;

        /* Move a batch of slots to amortize lock acquisition cost. */
        while (pool->global_free_ && transferred < DMA_POOL_PCP_BATCH) {
            DmaPoolSlotHdr* hdr = pool->global_free_;
            pool->global_free_ = hdr->next_;

            hdr->next_ = pcp->free_list_;
            pcp->free_list_ = hdr;
            
            pcp->count_++;
            transferred++;
        }

        DmaPoolSlotHdr* mine = pcp->free_list_;
        pcp->free_list_ = mine->next_;
        pcp->count_--;

        spinlock_release(&pool->lock_);
        irq_restore(irq_flags);

        *out_phys = mine->phys_;
        return (uint8_t*)mine + pool->obj_off_;
    }

    /*
     * Global pool is empty. We must ask the system for a new DMA page.
     */
    if (spinlock_try_acquire(&pool->grow_lock_)) {

        spinlock_release(&pool->lock_);

        DmaPoolPage* rec = 0;
        DmaPoolSlotHdr* local_head = 0;
        DmaPoolSlotHdr* local_tail = 0;

        const int ok = dma_pool_prepare_page(pool, &rec, &local_head, &local_tail);

        spinlock_acquire(&pool->lock_);

        if (likely(ok)) {
            local_tail->next_ = pool->global_free_;
            pool->global_free_ = local_head;

            dlist_add_tail(&rec->node_, &pool->pages_);
        }

        spinlock_release(&pool->grow_lock_);

        if (unlikely(!ok)) {
            spinlock_release(&pool->lock_);

            irq_restore(irq_flags);
            return 0;
        }

        goto retry_global;
    }

    /*
     * Another CPU is currently growing the pool.
     * Drop the lock and spin gently until the new page is integrated.
     */
    spinlock_release(&pool->lock_);

    spinlock_acquire(&pool->grow_lock_);
    spinlock_release(&pool->grow_lock_);

    spinlock_acquire(&pool->lock_);
    goto retry_global;
}

void dma_pool_free(dma_pool_t* pool, void* vaddr) {
    if (unlikely(!pool || !vaddr)) {
        return;
    }

    uint8_t* obj = (uint8_t*)vaddr;
    DmaPoolSlotHdr* hdr = (DmaPoolSlotHdr*)(obj - pool->obj_off_);

    uint32_t irq_flags = irq_save();

    const int cpu = hal_cpu_index();
    PerCpuDmaCache* pcp = &pool->cpu_cache_[cpu];

    hdr->next_ = pcp->free_list_;
    pcp->free_list_ = hdr;
    pcp->count_++;

    /*
     * Flush a batch to the global list if the per-CPU magazine overflows.
     * This prevents a single CPU from hoarding all free slots.
     */
    if (unlikely(pcp->count_ >= DMA_POOL_PCP_MAX)) {
        DmaPoolSlotHdr* drain_head = pcp->free_list_;
        DmaPoolSlotHdr* drain_tail = drain_head;

        for (uint32_t i = 1u; i < DMA_POOL_PCP_BATCH; i++) {
            drain_tail = drain_tail->next_;
        }

        pcp->free_list_ = drain_tail->next_;
        pcp->count_ -= DMA_POOL_PCP_BATCH;

        spinlock_acquire(&pool->lock_);

        drain_tail->next_ = pool->global_free_;
        pool->global_free_ = drain_head;

        spinlock_release(&pool->lock_);
    }

    irq_restore(irq_flags);
}

void dma_pool_destroy(dma_pool_t* pool) {
    if (unlikely(!pool)) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&pool->lock_);

    /*
     * Extract the entire page list into a local variable so we can safely
     * unmap and free them without holding the global spinlock.
     */
    dlist_head_t pages_to_free;
    dlist_init(&pages_to_free);

    if (!dlist_empty(&pool->pages_)) {
        pages_to_free.next = pool->pages_.next;
        pages_to_free.prev = pool->pages_.prev;

        pages_to_free.next->prev = &pages_to_free;
        pages_to_free.prev->next = &pages_to_free;

        dlist_init(&pool->pages_);
    }

    pool->global_free_ = 0;

    for (int i = 0; i < MAX_CPUS; i++) {
        pool->cpu_cache_[i].free_list_ = 0;
        pool->cpu_cache_[i].count_ = 0u;
    }

    spinlock_release_safe(&pool->lock_, flags);

    /* Now perform the heavy unmapping unhindered. */
    dlist_head_t* it = pages_to_free.next;
    while (it && it != &pages_to_free) {
        DmaPoolPage* p = container_of(it, DmaPoolPage, node_);
        it = it->next;

        if (p->vaddr_) {
            dma_free_coherent(p->vaddr_, PAGE_SIZE, p->phys_);
        }

        kfree(p);
    }

    if (pool->name_) {
        kfree(pool->name_);
        pool->name_ = 0;
    }

    kfree(pool);
}
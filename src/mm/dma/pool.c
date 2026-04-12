/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/output/kprintf.h>
#include <kernel/smp/cpu_limits.h>

#include <lib/tagged_ptr.h>
#include <lib/compiler.h>
#include <lib/lfslist.h>
#include <lib/lflist.h>
#include <lib/string.h>
#include <lib/dlist.h>

#include <hal/align.h>
#include <hal/lock.h>
#include <hal/cpu.h>
#include <hal/irq.h>

#include <mm/heap.h>

#include "api.h"

#ifndef PAGE_SIZE

#define PAGE_SIZE 4096u

#endif

#define DMA_POOL_PCP_MAX 32u
#define DMA_POOL_PCP_BATCH 16u

#define DMA_POOL_PCP_BATCH_MIN 4u
#define DMA_POOL_PCP_BATCH_MAX (DMA_POOL_PCP_MAX / 2u)
#define DMA_POOL_ADAPT_PERIOD 256u

/* Important clarification: any hard irq handlers cannot call dma_pool_*
 * because these internal functions do not save the eflags state
 * which is done to keep the fast allocation path fast.
 */

typedef struct DmaPoolPage {
    lfsnode_t node_;

    void* vaddr_;
    uint32_t phys_;
} DmaPoolPage;

typedef struct DmaPoolSlotHdr {
    lfnode_t node_;

    uint32_t phys_;
} DmaPoolSlotHdr;

typedef struct PerCpuDmaCache {
    lfnode_t* free_list_;

    uint32_t count_;

    uint32_t batch_size_;

    uint32_t hits_;
    uint32_t total_;

    uint8_t  _pad[64 - 5 * sizeof(uint32_t)];
} __cacheline_aligned PerCpuDmaCache;

struct dma_pool {
    /* hot data for per-cpu local allocation */
    /* cacheline 1 */

    uint32_t obj_off_;
    uint32_t slot_size_;

    /* slow path data */

    lflist_head_t global_free_;

    uint8_t _pad0[52];

    /* cacheline 2 for cold data */

    spinlock_t   grow_lock_;

    lfslist_head_t pages_;

    uint32_t obj_size_;
    uint32_t align_;

    uint32_t boundary_;

    uint32_t nr_active_;

    char* name_;

    uint8_t _pad1[36];

    /* cacheline 3 */

    PerCpuDmaCache cpu_cache_[MAX_CPUS];
} __cacheline_aligned;

___inline int is_pow2(size_t v) {
    return v && ((v & (v - 1u)) == 0u);
}

/*
 * Allocate a new backing DMA page from the system and carve it into slots.
 * This runs entirely lockless with respect to the pool's internal locks.
 */
___noinline static int dma_pool_prepare_page(
    dma_pool_t* pool, DmaPoolPage** out_page,
    lfnode_t** out_head, lfnode_t** out_tail 
) {
    if (unlikely(!pool || !out_page || !out_head || !out_tail)) {
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

    if (unlikely((page_phys & (uint32_t)(pool->align_ - 1u)) != 0u)) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        return 0;
    }

    DmaPoolPage* rec = (DmaPoolPage*)kzalloc(sizeof(*rec));
    if (unlikely(!rec)) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        return 0;
    }

    rec->node_.next_ = 0;
    rec->vaddr_ = page;
    rec->phys_  = page_phys;

    const uint32_t slot_size = pool->slot_size_;
    const uint32_t obj_off   = pool->obj_off_;
    const uint32_t boundary  = pool->boundary_;

    uint32_t offset       = 0u;
    uint32_t next_boundary = boundary;

    uint8_t*  slot_ptr = (uint8_t*)page;
    uint32_t  phys_cur = page_phys + obj_off;

    lfnode_t* head = 0;
    lfnode_t* tail = 0;

    while (offset + slot_size <= PAGE_SIZE) {
        if (unlikely(offset + slot_size > next_boundary)) {
            const uint32_t skip = next_boundary - offset;

            offset = next_boundary;
            next_boundary += boundary;

            slot_ptr += skip;
            phys_cur += skip;

            continue;
        }

        DmaPoolSlotHdr* hdr = (DmaPoolSlotHdr*)slot_ptr;

        hdr->phys_ = phys_cur;
        hdr->node_.next_ = 0;

        if (!head) {
            head = &hdr->node_;
        } else {
            tail->next_ = &hdr->node_;
        }

        tail = &hdr->node_;

        offset   += slot_size;
        slot_ptr += slot_size;
        phys_cur += slot_size;
    }

    if (unlikely(!head)) {
        dma_free_coherent(page, PAGE_SIZE, page_phys);
        kfree(rec);
        return 0;
    }

    *out_page = rec;
    *out_head = head;
    *out_tail = tail;

    return 1;
}

dma_pool_t* dma_pool_create(const char* name, size_t obj_size, size_t align, size_t boundary) {
    if (unlikely(!name || obj_size == 0u)) {
        kprintf("dma_pool_create: called on null name pointer or null obj size");
        return 0;
    }

    if (align == 0u) {
        align = sizeof(void*);
    }

    if (unlikely(!is_pow2(align) || align > PAGE_SIZE)) {
        return 0;
    }

    /*
     * This means there are no restrictions
     * simplify the cutting logic by assigning a page size.
     */
    if (boundary == 0u) {
        boundary = PAGE_SIZE;
    }

    if (unlikely(!is_pow2(boundary) || boundary < obj_size)) {
        return 0;
    }

    /* boundary cannot be larger than the page size. */
    if (boundary > PAGE_SIZE) {
        boundary = PAGE_SIZE;
    }

    dma_pool_t* pool = (dma_pool_t*)kzalloc(sizeof(*pool));
    if (unlikely(!pool)) {
        return 0;
    }

    pool->name_ = strdup(name);
    if (unlikely(!pool->name_)) {
        kfree(pool);
        return 0;
    }

    pool->obj_size_ = obj_size;
    pool->align_    = align;
    
    pool->boundary_ = boundary;
    pool->nr_active_ = 0u;

    pool->obj_off_   = align_up(sizeof(DmaPoolSlotHdr), align);
    pool->slot_size_ = align_up(pool->obj_off_ + obj_size, align);

    if (unlikely(pool->slot_size_ == 0u || pool->slot_size_ > PAGE_SIZE)) {
        kfree(pool->name_);
        kfree(pool);
        return 0;
    }

    spinlock_init(&pool->grow_lock_);

    lflist_init(&pool->global_free_);

    lfslist_init(&pool->pages_);

    for (int i = 0; i < MAX_CPUS; i++) {
        pool->cpu_cache_[i].free_list_  = 0;
        pool->cpu_cache_[i].count_      = 0u;
        pool->cpu_cache_[i].batch_size_ = DMA_POOL_PCP_BATCH;
        pool->cpu_cache_[i].hits_       = 0u;
        pool->cpu_cache_[i].total_      = 0u;
    }

    return pool;
}

void* dma_pool_alloc(dma_pool_t* pool, uint32_t* out_phys) {
    if (unlikely(!pool || !out_phys)) {
        kprintf("dma_pool_alloc: called on null pool pointer or null out_phys");
        return 0;
    }
    /*
     * Do not disable interrupts in the fast path, because the kernel is not preemptible
     * Cpu index cannot be invalidated at the time of allocation.
     */

    const int cpu = hal_cpu_index();
    PerCpuDmaCache* pcp = &pool->cpu_cache_[cpu];

    /*
     * Fast path: serve directly from the per-CPU magazine.
     */
    if (likely(pcp->count_ > 0u)) {
        lfnode_t* node = pcp->free_list_;

        if (likely(node->next_)) {
            __builtin_prefetch(node->next_, 0, 3);
        }

        pcp->free_list_ = node->next_;
        pcp->count_--;

        pcp->hits_++;
        pcp->total_++;

        if (unlikely(pcp->total_ >= DMA_POOL_ADAPT_PERIOD)) {
            const uint32_t hits  = pcp->hits_;
            const uint32_t total = pcp->total_;

            pcp->hits_  = 0u;
            pcp->total_ = 0u;

            const uint32_t hit_rate = (hits * 100u) / total;

            if (hit_rate < 70u) {
                const uint32_t nb   = pcp->batch_size_ << 1u;
                pcp->batch_size_    = nb > DMA_POOL_PCP_BATCH_MAX
                                    ? DMA_POOL_PCP_BATCH_MAX
                                    : nb;
            } else if (hit_rate > 95u) {
                const uint32_t nb   = pcp->batch_size_ >> 1u;
                pcp->batch_size_    = nb < DMA_POOL_PCP_BATCH_MIN
                                    ? DMA_POOL_PCP_BATCH_MIN
                                    : nb;
            }
        }

        __atomic_fetch_add(&pool->nr_active_, 1u, __ATOMIC_RELAXED);

        DmaPoolSlotHdr* hdr = container_of(node, DmaPoolSlotHdr, node_);

        *out_phys = hdr->phys_;
        return (uint8_t*)hdr + pool->obj_off_;
    }

    pcp->total_++;

    /*
     * Slow path: replenish the per-CPU magazine from the global pool.
     */
    uint32_t transferred = 0u;
    lfnode_t* batch = lflist_pop_batch(
        &pool->global_free_, pcp->batch_size_,
        &transferred
    );

    if (likely(batch)) {
        /* Move a batch of slots to amortize lock acquisition cost. */
        lfnode_t* cur = batch;
        while (cur) {
            lfnode_t* next = cur->next_;

            cur->next_ = pcp->free_list_;
            pcp->free_list_ = cur;

            pcp->count_++;
            cur = next;
        }

        lfnode_t* mine_node = pcp->free_list_;

        pcp->free_list_ = mine_node->next_;
        pcp->count_--;

        DmaPoolSlotHdr* mine = container_of(mine_node, DmaPoolSlotHdr, node_);

        __atomic_fetch_add(&pool->nr_active_, 1u, __ATOMIC_RELAXED);
        
        *out_phys = mine->phys_;

        return (uint8_t*)mine + pool->obj_off_;
    }

    /*
     * Global pool is empty. We must ask the system for a new DMA page.
     */
    uint32_t irq_flags = irq_save();

    if (spinlock_try_acquire(&pool->grow_lock_)) {
        DmaPoolPage*    rec        = 0;

        lfnode_t*    local_head = 0;
        lfnode_t*    local_tail = 0;

        const int ok = dma_pool_prepare_page(pool, &rec, &local_head, &local_tail);

        if (likely(ok)) {
            lflist_push_batch(&pool->global_free_, local_head, local_tail);

            lfslist_push(&pool->pages_, &rec->node_);
        }

        spinlock_release(&pool->grow_lock_);

        irq_restore(irq_flags);

        if (unlikely(!ok)) {
            return 0;
        }

        lfnode_t* node = lflist_pop(&pool->global_free_);
        if (unlikely(!node)) {
            return 0;
        }

        DmaPoolSlotHdr* hdr = container_of(node, DmaPoolSlotHdr, node_);

        __atomic_fetch_add(&pool->nr_active_, 1u, __ATOMIC_RELAXED);

        *out_phys = hdr->phys_;

        return (uint8_t*)hdr + pool->obj_off_;
    }

    /*
     * Another CPU is currently growing the pool.
     * Drop the lock and spin gently until the new page is integrated.
     */
    spinlock_acquire(&pool->grow_lock_);

    spinlock_release(&pool->grow_lock_);
    
    irq_restore(irq_flags);

    lfnode_t* node = lflist_pop(&pool->global_free_);
    if (unlikely(!node)) {
        return 0;
    }

    DmaPoolSlotHdr* hdr = container_of(node, DmaPoolSlotHdr, node_);

    __atomic_fetch_add(&pool->nr_active_, 1u, __ATOMIC_RELAXED);

    *out_phys = hdr->phys_;

    return (uint8_t*)hdr + pool->obj_off_;
}

void dma_pool_free(dma_pool_t* pool, void* vaddr) {
    if (unlikely(!pool || !vaddr)) {
        kprintf("dma_pool_free: called on null pool pointer or null vaddr");
        return;
    }

    __atomic_fetch_sub(&pool->nr_active_, 1u, __ATOMIC_RELAXED);

    uint8_t* obj = (uint8_t*)vaddr;
    DmaPoolSlotHdr* hdr = (DmaPoolSlotHdr*)(obj - pool->obj_off_);

    /*
     * Do not disable interrupts while using the CPU index
     * see the note in dma_pool_alloc.
     */
    const int cpu = hal_cpu_index();
    PerCpuDmaCache* pcp = &pool->cpu_cache_[cpu];

    if (likely(pcp->free_list_)) {
        __builtin_prefetch(pcp->free_list_, 0, 3); /* to avoid cache misses later */
    }

    hdr->node_.next_ = pcp->free_list_;

    pcp->free_list_ = &hdr->node_;
    pcp->count_++;

    /*
     * Flush a batch to the global list if the per-CPU magazine overflows.
     * This prevents a single CPU from hoarding all free slots.
     */
    if (unlikely(pcp->count_ >= pcp->batch_size_)) {
        lfnode_t* drain_head = pcp->free_list_;
        lfnode_t* drain_tail = drain_head;

        for (uint32_t i = 1u; i < pcp->batch_size_; i++) {
            drain_tail = drain_tail->next_;
        }

        pcp->free_list_ = drain_tail->next_;
        pcp->count_ -= pcp->batch_size_;
        
        lflist_push_batch(&pool->global_free_, drain_head, drain_tail);
    }
}

void dma_pool_destroy(dma_pool_t* pool) {
    if (unlikely(!pool)) {
        kprintf("dma_pool_destroy: called on null pool pointer\n");
        return;
    }

    if (unlikely(pool->nr_active_ != 0u)) {
        kprintf(
            "dma_pool_destroy: pool '%s' destroyed with %u active allocations\n",
            pool->name_ ? pool->name_ : "?", pool->nr_active_
        );
    }

    /*
     * Extract the page list. Since destroy() implies exclusive access 
     * to the pool context, we don't need to acquire grow_lock_ here.
     */
    lfsnode_t* it = lfslist_pop_all(&pool->pages_);

    lfslist_init(&pool->pages_);

    lflist_init(&pool->global_free_);

    for (int i = 0; i < MAX_CPUS; i++) {
        pool->cpu_cache_[i].free_list_  = 0;
        pool->cpu_cache_[i].count_      = 0u;
        pool->cpu_cache_[i].batch_size_ = 0u;
        pool->cpu_cache_[i].hits_       = 0u;
        pool->cpu_cache_[i].total_      = 0u;
    }

    while (it) {
        lfsnode_t* next = it->next_;
        
        DmaPoolPage* page = container_of(it, DmaPoolPage, node_);

        if (likely(page->vaddr_)) {
            dma_free_coherent(page->vaddr_, PAGE_SIZE, page->phys_);
        }

        kfree(page);
        
        it = next;
    }

    if (likely(pool->name_)) {
        kfree(pool->name_);
    }

    kfree(pool);
}
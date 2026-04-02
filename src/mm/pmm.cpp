/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <lib/compiler.h>

#include <lib/cpp/lock_guard.h>

#include "pmm.h"

#include <kernel/smp/cpu.h>
#include <kernel/rcu.h>

namespace kernel {

namespace {

/*
 * Keep order-0 traffic off the buddy fast path.
 * Hit rate matters more than perfect global accounting here.
 */
struct __cacheline_aligned PerCpuPageCache {
    uint32_t count = 0u;
    void* phys_pages[256]{};
};

static constexpr uint32_t pcp_capacity = 256u;
static constexpr uint32_t pcp_refill_batch = 32u;
static constexpr uint32_t pcp_drain_batch = 128u;

static constexpr uint32_t pcp_refill_order = 5u;

static PerCpuPageCache pcp_caches[MAX_CPUS][PMM_ZONE_COUNT]{};

static inline uint32_t pcp_cpu_index() {
    cpu_t* cpu = cpu_current();
    if (!cpu) {
        return 0u;
    }

    const int idx = cpu->index;
    if (idx < 0
        || idx >= MAX_CPUS) {
        return 0u;
    }

    return static_cast<uint32_t>(idx);
}

static inline PerCpuPageCache& pcp_current_cache(pmm_zone_t zone) {
    return pcp_caches[pcp_cpu_index()][zone];
}

/*
 * Drain in batches to keep the cache hot and to amortize the buddy lock.
 * Full cache is treated as backpressure: do not let per-cpu caches grow
 * unbounded under a free-heavy workload.
 */
static inline void pcp_cache_drain(PmmState* pmm, PerCpuPageCache& cache) {
    if (!pmm
        || cache.count < pcp_capacity) {
        return;
    }

    void* batch[pcp_drain_batch] = {};

    const uint32_t drain = (cache.count < pcp_drain_batch)
        ? cache.count
        : pcp_drain_batch;

    for (uint32_t i = 0u; i < drain; i++) {
        batch[i] = cache.phys_pages[cache.count - 1u - i];
    }

    cache.count -= drain;
    pmm->free_pages_order0_batch(batch, drain);
}

static inline void* pcp_refill_fallback(
    PmmState* pmm, pmm_zone_t zone,
    PerCpuPageCache& cache
) {
    void* batch[pcp_refill_batch] = {};
    const uint32_t n = pmm->alloc_pages_order0_batch(zone, batch, pcp_refill_batch);
    if (kernel::unlikely(n == 0u)) {
        return nullptr;
    }

    /*
     * Return one page immediately and cache the rest.
     * This keeps the caller fast while still amortizing the buddy work.
     */
    for (uint32_t i = 1u;
         i < n
         && cache.count < pcp_capacity;
         i++) {

        cache.phys_pages[cache.count++] = batch[i];
    }

    return batch[0];
}

static inline void pcp_fix_shattered_page_metadata(page_t& page, pmm_zone_t zone) {
    /*
     * Shattered pages never pass through the buddy split path.
     * Fix metadata to match the state of a normal order-0 allocation:
     * USED, ref_count=1, no freelist links, no slab association.
     */
    page.flags |= PMM_FLAG_USED;
    page.flags &= ~PMM_FLAG_FREE;

    if (zone == PMM_ZONE_DMA) {
        page.flags |= PMM_FLAG_DMA;
    } else {
        page.flags &= ~PMM_FLAG_DMA;
    }

    page.ref_count = 1;

    page.slab_cache = nullptr;
    page.freelist = nullptr;
    page.objects = 0;

    page.list.prev = nullptr;
    page.list.next = nullptr;
}

static inline void* pcp_alloc_from_cache(PmmState* pmm, pmm_zone_t zone) {
    /*
     * Disable IRQs to pin execution on the current CPU.
     * This keeps the per-cpu cache lockless without requiring preemption
     * control in early boot paths.
     */
    kernel::ScopedIrqDisable irq_guard;

    PerCpuPageCache& cache = pcp_current_cache(zone);
    if (kernel::likely(cache.count > 0u)) {
        return cache.phys_pages[--cache.count];
    }

    void* big_block = pmm->alloc_pages_zone(pcp_refill_order, zone);
    if (kernel::unlikely(!big_block)) {
        return pcp_refill_fallback(pmm, zone, cache);
    }

    /*
     * Prefer shattering a larger block over taking 32 independent order-0
     * allocations under the buddy lock. This trades a single split for a
     * predictable refill cost.
     */
    uint8_t* base = static_cast<uint8_t*>(big_block);

    for (uint32_t i = 1u; i < pcp_refill_batch; i++) {
        void* phys = base + (i * PAGE_SIZE);

        page_t* meta = pmm->phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(phys)));
        if (kernel::unlikely(!meta)) {
            continue;
        }

        pcp_fix_shattered_page_metadata(*meta, zone);
        cache.phys_pages[cache.count++] = phys;
    }

    page_t* head_meta = pmm->phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)));
    if (kernel::likely(head_meta)) {
        pcp_fix_shattered_page_metadata(*head_meta, zone);
    }

    return base;
}

static inline void pcp_free_to_cache(PmmState* pmm, pmm_zone_t zone, void* addr) {
    /* See pcp_alloc_from_cache(): irq-off keeps the cache lockless. */
    kernel::ScopedIrqDisable irq_guard;

    PerCpuPageCache& cache = pcp_current_cache(zone);
    if (cache.count >= pcp_capacity) {
        pcp_cache_drain(pmm, cache);
    }

    if (cache.count < pcp_capacity) {
        cache.phys_pages[cache.count++] = addr;
    } else {
        pmm->free_pages_order0_batch(&addr, 1u);
    }
}

}

alignas(PmmState) static unsigned char g_pmm_storage[sizeof(PmmState)];
static PmmState* g_pmm = nullptr;

static constexpr uint32_t k_dma_limit = 16u * 1024u * 1024u;

PmmState* pmm_state() noexcept {
    return g_pmm;
}

void PmmState::init(uint32_t mem_size, uint32_t kernel_end_addr) noexcept {
    pmm_region_t region = {};
    region.base = 0u;
    region.size = mem_size;
    region.type = PMM_REGION_AVAILABLE;

    init_regions(&region, 1u, nullptr, 0u, kernel_end_addr);
}

void PmmState::init_regions(
    const pmm_region_t* regions, uint32_t region_count,
    const pmm_reserved_region_t* reserved, uint32_t reserved_count, uint32_t kernel_end_addr
) noexcept {
    /*
     * Start fully pessimistic: mark everything used first, then selectively
     * free only what the memory map explicitly allows.
     */
    total_pages_ = 0u;
    for (uint32_t i = 0u; i < MAX_CPUS; i++) {
        cpu_used_pages_[i] = 0u;
    }
    mem_map_ = nullptr;

    for (uint32_t zone = 0u; zone < PMM_ZONE_COUNT; zone++) {
        for (uint32_t order = 0u; order <= PMM_MAX_ORDER; order++) {
            zones_[zone].free_areas[order] = {};
        }

        zones_[zone].free_bitmap = 0u;
    }

    if (!regions
        || region_count == 0u) {
        return;
    }

    /*
     * Size the mem_map by the highest address any AVAILABLE region reaches.
     * This keeps PFN indexing simple and avoids sparse metadata.
     */
    uint32_t max_end = 0u;
    for (uint32_t i = 0u; i < region_count; i++) {
        if (regions[i].type != PMM_REGION_AVAILABLE) {
            continue;
        }

        const uint32_t base = regions[i].base;
        const uint32_t size = regions[i].size;

        if (size == 0u) {
            continue;
        }

        uint32_t end = base + size;
        if (end < base) {
            end = 0xFFFFFFFFu;
        }

        if (end > max_end) {
            max_end = end;
        }
    }

    const uint32_t max_addr = align_down(max_end);
    total_pages_ = max_addr / PAGE_SIZE;
    if (total_pages_ == 0u) {
        return;
    }

    /*
     * Place the page metadata immediately after the kernel image.
     * Treat it as reserved: free_range() must never hand these pages out.
     */
    const uint32_t mem_map_phys = align_up(kernel_end_addr);
    mem_map_ = reinterpret_cast<page_t*>(mem_map_phys);

    const uint32_t mem_map_size = total_pages_
        * static_cast<uint32_t>(sizeof(page_t));

    const uint32_t mem_map_end = align_up(mem_map_phys + mem_map_size);
    if (mem_map_end < mem_map_phys) {
        /* mem_map would overflow. */
        mem_map_ = nullptr;
        total_pages_ = 0u;

        return;
    }

    memset(mem_map_, 0, mem_map_size);

    init_used_pages(total_pages_, mem_map_end);

    for (uint32_t i = 0u; i < region_count; i++) {
        if (regions[i].type != PMM_REGION_AVAILABLE) {
            continue;
        }

        uint32_t start = align_up(regions[i].base);
        uint32_t end = align_down(regions[i].base + regions[i].size);

        if (end <= start) {
            continue;
        }

        if (start < mem_map_end) {
            /* Skip the metadata itself, even if it lies inside an AVAILABLE region. */
            start = mem_map_end;
        }

        if (end <= start) {
            continue;
        }

        free_range(start, end, 0u, reserved, reserved_count);
    }
}

void* PmmState::alloc_pages(uint32_t order) noexcept {
    if (order == 0u) {
        /*
         * Keep the fast path biased towards NORMAL.
         * DMA pages are a constrained resource; only dip into it when NORMAL
         * cannot satisfy the request.
         */
        void* p = pcp_alloc_from_cache(this, PMM_ZONE_NORMAL);
        if (p) {
            return p;
        }

        return pcp_alloc_from_cache(this, PMM_ZONE_DMA);
    }

    void* ptr = alloc_pages_zone(order, PMM_ZONE_NORMAL);
    if (ptr) {
        return ptr;
    }

    return alloc_pages_zone(order, PMM_ZONE_DMA);
}

void* PmmState::alloc_pages_zone(uint32_t order, pmm_zone_t zone) noexcept {
    if (order == 0u) {
        if (kernel::unlikely(zone >= PMM_ZONE_COUNT)) {
            return nullptr;
        }

        return pcp_alloc_from_cache(this, zone);
    }

    SpinLockSafeGuard guard(zones_[zone].lock);

    return alloc_pages_zone_unlocked(order, zone);
}

void* PmmState::alloc_pages_zone_unlocked(uint32_t order, pmm_zone_t zone) noexcept {
    if (kernel::unlikely(order > PMM_MAX_ORDER)) {
        return nullptr;
    }

    if (kernel::unlikely(zone >= PMM_ZONE_COUNT)) {
        return nullptr;
    }

    /*
     * `free_bitmap` is a quick reject/selector for non-empty orders.
     * Keep it consistent with `free_areas[].count` updates.
     */
    const uint32_t bitmap = zones_[zone].free_bitmap;
    const uint32_t order_mask = (order == 0u) ? 0u : ((1u << order) - 1u);
    const uint32_t available = bitmap & ~order_mask;

    if (kernel::unlikely(available == 0u)) {
        return nullptr;
    }

    /*
     * Find the smallest usable order quickly.
     * The bitmap encodes which orders have at least one free block.
     */
    uint32_t current_order = static_cast<uint32_t>(__builtin_ctz(available));

    page_t* page = free_area_pop(zone, current_order);
    if (kernel::unlikely(!page)) {
        return nullptr;
    }

    page->flags |= PMM_FLAG_USED;
    page->flags &= ~PMM_FLAG_FREE;
    page->ref_count = 1;

    page->slab_cache = nullptr;

    page->freelist = nullptr;
    page->objects = 0;

    page->list.prev = nullptr;
    page->list.next = nullptr;

    /*
     * Split down by repeatedly carving out the upper buddy and returning it
     * to the appropriate freelist.
     *
     * Invariant: `page` always points at the lower PFN of the current block.
     */
    while (current_order > order) {
        current_order--;

        const uint32_t pfn = static_cast<uint32_t>(page - mem_map_);
        const uint32_t buddy_pfn = pfn + (1u << current_order);

        page_t* buddy = &mem_map_[buddy_pfn];

        buddy->flags = (buddy->flags | PMM_FLAG_FREE) & ~PMM_FLAG_USED;
        buddy->order = current_order;
        buddy->ref_count = 0;

        free_area_push(zone, current_order, buddy);
    }

    page->order = order;

    cpu_used_pages_[pcp_cpu_index()] += (1u << order);

    return reinterpret_cast<void*>(page_to_phys(page));
}

uint32_t PmmState::alloc_pages_order0_batch(
    pmm_zone_t preferred,
    void** out, uint32_t cap
) noexcept {
    if (kernel::unlikely(!out
        || cap == 0u)) {
        return 0u;
    }

    if (kernel::unlikely(preferred >= PMM_ZONE_COUNT)) {
        preferred = PMM_ZONE_NORMAL;
    }

    pmm_zone_t fallback = PMM_ZONE_NORMAL;
    if (preferred == PMM_ZONE_NORMAL) {
        fallback = PMM_ZONE_DMA;
    }

    /*
     * Allocate under a single zone lock at a time.
     * This avoids lock ping-pong in refill paths and keeps ordering simple.
     */
    uint32_t n = 0u;

    {
        SpinLockSafeGuard guard(zones_[preferred].lock);

        for (; n < cap; n++) {
            void* p = alloc_pages_zone_unlocked(0u, preferred);
            if (!p) {
                break;
            }

            out[n] = p;
        }
    }

    if (n < cap) {
        SpinLockSafeGuard guard(zones_[fallback].lock);

        for (; n < cap; n++) {
            void* p = alloc_pages_zone_unlocked(0u, fallback);
            if (!p) {
                break;
            }

            out[n] = p;
        }
    }

    return n;
}

void PmmState::free_pages_order0_batch(void* const* pages, uint32_t n) noexcept {
    if (kernel::unlikely(!pages
        || n == 0u)) {
        return;
    }

    /*
     * Partition by zone first.
     * That gives two tight loops with one lock each, which is cheaper than
     * switching locks per page.
     */
    void* dma_pages[pcp_drain_batch] = {};
    void* normal_pages[pcp_drain_batch] = {};

    uint32_t dma_count = 0u;
    uint32_t normal_count = 0u;

    for (uint32_t i = 0u; i < n; i++) {
        if (kernel::unlikely(!pages[i])) {
            continue;
        }

        page_t* page = phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pages[i])));
        if (kernel::unlikely(!page)) {
            continue;
        }

        const pmm_zone_t zone = zone_for_flags(page->flags);
        if (zone == PMM_ZONE_DMA) {
            dma_pages[dma_count++] = pages[i];
        } else {
            normal_pages[normal_count++] = pages[i];
        }
    }

    if (dma_count > 0u) {
        SpinLockSafeGuard guard(zones_[PMM_ZONE_DMA].lock);

        for (uint32_t i = 0u; i < dma_count; i++) {
            free_pages_unlocked(dma_pages[i], 0u);
        }
    }

    if (normal_count > 0u) {
        SpinLockSafeGuard guard(zones_[PMM_ZONE_NORMAL].lock);

        for (uint32_t i = 0u; i < normal_count; i++) {
            free_pages_unlocked(normal_pages[i], 0u);
        }
    }
}

void PmmState::free_pages(void* addr, uint32_t order) noexcept {
    if (kernel::unlikely(!addr)) {
        return;
    }

    if (kernel::unlikely(order > PMM_MAX_ORDER)) {
        return;
    }

    page_t* page = phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)));

    if (kernel::unlikely(!page)) {
        return;
    }

    if (order == 0u) {
        if (kernel::unlikely((page->flags & PMM_FLAG_KERNEL) != 0u)) {
            return;
        }

        if (kernel::unlikely((page->flags & PMM_FLAG_USED) == 0u)) {
            return;
        }

        const pmm_zone_t zone = zone_for_flags(page->flags);

        pcp_free_to_cache(this, zone, addr);

        return;
    }

    const pmm_zone_t zone = zone_for_flags(page->flags);
    SpinLockSafeGuard guard(zones_[zone].lock);

    free_pages_unlocked(addr, order);
}

page_t* PmmState::phys_to_page(uint32_t phys_addr) noexcept {
    const uint32_t idx = phys_addr / PAGE_SIZE;

    if (kernel::unlikely(idx >= total_pages_)) {
        return nullptr;
    }

    return &mem_map_[idx];
}

uint32_t PmmState::page_to_phys(page_t* page) const noexcept {
    const uint32_t idx = static_cast<uint32_t>(page - mem_map_);

    return idx * PAGE_SIZE;
}

uint32_t PmmState::get_used_blocks() const noexcept {
    uint32_t total = 0u;
    for (uint32_t i = 0u; i < MAX_CPUS; i++) {
        total += cpu_used_pages_[i];
    }
    return total;
}

uint32_t PmmState::get_free_blocks() const noexcept {
    return total_pages_ - get_used_blocks();
}

uint32_t PmmState::get_total_blocks() const noexcept {
    return total_pages_;
}

uint32_t PmmState::align_up(uint32_t addr) noexcept {
    return PAGE_ALIGN(addr);
}

uint32_t PmmState::align_down(uint32_t addr) noexcept {
    return addr & ~(PAGE_SIZE - 1u);
}

void PmmState::init_used_pages(uint32_t total_pages, uint32_t kernel_end_addr) noexcept {
    cpu_used_pages_[0u] = total_pages;

    const uint32_t kernel_end = align_up(kernel_end_addr);

    for (uint32_t i = 0u; i < total_pages; i++) {
        const uint32_t addr = i * PAGE_SIZE;
        page_t& page = mem_map_[i];

        page.flags = PMM_FLAG_USED | zone_flags_for_addr(addr);
        if (addr < kernel_end) {
            page.flags |= PMM_FLAG_KERNEL;
        }

        page.ref_count = 1;
        page.order = 0;

        page.slab_cache = nullptr;
        page.freelist = nullptr;
        page.objects = 0;

        page.list.prev = nullptr;
        page.list.next = nullptr;
    }
}

void PmmState::free_range(
    uint32_t start, uint32_t end,
    uint32_t zone_flags,
    const pmm_reserved_region_t* reserved, uint32_t reserved_count
) noexcept {
    /*
     * Free the range using the largest blocks that fit.
     * This builds near-optimal initial freelists and avoids early fragmentation
     * that would later amplify buddy split pressure.
     */
    if (end <= start) {
        return;
    }

    uint32_t cur = align_up(start);
    const uint32_t end_aligned = align_down(end);

    if (end_aligned <= cur) {
        return;
    }

    if (zone_flags != 0u
        && (zone_flags & PMM_FLAG_DMA) == 0u
        && cur < k_dma_limit) {

        cur = k_dma_limit;
    }

    while (cur < end_aligned) {
        if (zone_flags != 0u
            && (zone_flags & PMM_FLAG_DMA) != 0u
            && cur >= k_dma_limit) {

            break;
        }

        uint32_t max_order = PMM_MAX_ORDER;
        while (max_order > 0u) {
            const uint32_t block_size = PAGE_SIZE << max_order;
            const uint32_t block_end = cur + block_size;

            if (block_end < cur
                || block_end > end_aligned) {
                max_order--;

                continue;
            }

            const uint32_t cur_zone_flags = (zone_flags != 0u)
                ? zone_flags
                : zone_flags_for_addr(cur);

            if ((cur_zone_flags & PMM_FLAG_DMA) != 0u) {
                if (block_end > k_dma_limit) {
                    max_order--;

                    continue;
                }
            } else if (cur < k_dma_limit
                && block_end > k_dma_limit) {
                max_order--;

                continue;
            }

            if (range_is_reserved(cur, block_end, reserved, reserved_count)) {
                max_order--;

                continue;
            }

            break;
        }

        const uint32_t block_size = PAGE_SIZE << max_order;
        const uint32_t block_end = cur + block_size;

        if (block_end <= cur
            || block_end > end_aligned) {
            break;
        }

        if (range_is_reserved(cur, block_end, reserved, reserved_count)) {
            /*
             * Do not skip a whole block here.
             * If the current best-fit block overlaps reserved memory, walk a
             * single page forward and retry to avoid punching holes.
             */
            cur += PAGE_SIZE;
            continue;
        }

        free_pages_unlocked(reinterpret_cast<void*>(cur), max_order);
        cur = block_end;
    }
}

bool PmmState::range_is_reserved(
    uint32_t start, uint32_t end,
    const pmm_reserved_region_t* reserved,
    uint32_t reserved_count
) const noexcept {
    if (!reserved
        || reserved_count == 0u) {
        return false;
    }

    /*
     * Match allocator granularity.
     * Callers probe ranges aligned to page/block boundaries, so normalize
     * reserved regions the same way to keep comparisons stable.
     */
    for (uint32_t i = 0u; i < reserved_count; i++) {

        const uint32_t base = reserved[i].base;
        const uint32_t size = reserved[i].size;

        if (size == 0u) {
            continue;
        }

        const uint32_t r_start = align_down(base);
        uint32_t r_end = base + size;

        if (r_end < base) {
            r_end = 0xFFFFFFFFu;
        }
        r_end = align_up(r_end);

        if (r_end <= start) {
            continue;
        }

        if (r_start >= end) {
            continue;
        }

        return true;
    }

    return false;
}

uint32_t PmmState::zone_flags_for_addr(uint32_t addr) noexcept {
    if (addr < k_dma_limit) {
        return PMM_FLAG_DMA;
    }

    return 0u;
}

pmm_zone_t PmmState::zone_for_flags(uint32_t flags) noexcept {
    if ((flags & PMM_FLAG_DMA) != 0u) {
        return PMM_ZONE_DMA;
    }

    return PMM_ZONE_NORMAL;
}

void PmmState::list_add(page_t** head, page_t* page) noexcept {
    page->list.next = *head;
    page->list.prev = nullptr;

    if (*head) {
        (*head)->list.prev = page;
    }

    *head = page;
}

void PmmState::list_remove(page_t** head, page_t* page) noexcept {
    if (page->list.prev) {
        page->list.prev->list.next = page->list.next;
    } else {
        *head = page->list.next;
    }

    if (page->list.next) {
        page->list.next->list.prev = page->list.prev;
    }

    page->list.next = nullptr;
    page->list.prev = nullptr;
}

void PmmState::free_area_push(pmm_zone_t zone, uint32_t order, page_t* page) noexcept {
    FreeArea& area = zones_[zone].free_areas[order];

    const uint32_t prev_count = area.count;

    list_add(&area.head, page);

    area.count = prev_count + 1u;

    if (prev_count == 0u) {
        /* Keep `free_bitmap` in sync with the list becoming non-empty. */
        zones_[zone].free_bitmap |= 1u << order;
    }
}

page_t* PmmState::free_area_pop(pmm_zone_t zone, uint32_t order) noexcept {
    FreeArea& area = zones_[zone].free_areas[order];

    page_t* page = area.head;

    if (!page) {
        return nullptr;
    }

    list_remove(&area.head, page);
    area.count--;

    if (area.count == 0u) {
        /* Keep `free_bitmap` in sync with the list becoming empty. */
        zones_[zone].free_bitmap &= ~(1u << order);
    }

    return page;
}

void PmmState::free_area_remove(pmm_zone_t zone, uint32_t order, page_t* page) noexcept {
    FreeArea& area = zones_[zone].free_areas[order];

    list_remove(&area.head, page);
    area.count--;

    if (area.count == 0u) {
        /* Keep `free_bitmap` in sync with the list becoming empty. */
        zones_[zone].free_bitmap &= ~(1u << order);
    }
}

void PmmState::free_pages_unlocked(void* addr, uint32_t order) noexcept {
    page_t* page = phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)));

    if (!page) {
        return;
    }

    if ((page->flags & PMM_FLAG_USED) == 0u) {
        return;
    }

    if ((page->flags & PMM_FLAG_KERNEL) != 0u) {
        return;
    }

    cpu_used_pages_[pcp_cpu_index()] -= (1u << order);

    uint32_t pfn = static_cast<uint32_t>(page - mem_map_);

    const pmm_zone_t zone = zone_for_flags(page->flags);

    /*
     * Merge rules are strict.
     *
     * The buddy must:
     * - be free (not USED)
     * - have the same order
     * - belong to the same zone
     *
     * If any condition fails, stop merging. Anything else corrupts freelists.
     */
    while (order < PMM_MAX_ORDER) {
        const uint32_t buddy_pfn = pfn ^ (1u << order);

        if (buddy_pfn >= total_pages_) {
            break;
        }

        page_t* buddy = &mem_map_[buddy_pfn];

        if ((buddy->flags & PMM_FLAG_USED) != 0u) {
            break;
        }

        if (buddy->order != order) {
            break;
        }

        if (zone_for_flags(buddy->flags) != zone) {
            break;
        }

        free_area_remove(zone, order, buddy);

        buddy->order = 0;

        pfn &= buddy_pfn;
        page = &mem_map_[pfn];

        order++;
    }

    page->slab_cache = nullptr;
    page->freelist = nullptr;
    page->objects = 0;
    page->list.prev = nullptr;
    page->list.next = nullptr;

    page->flags = (page->flags | PMM_FLAG_FREE) & ~PMM_FLAG_USED;
    page->order = order;
    page->ref_count = 0;

    free_area_push(zone, order, page);
}

static inline PmmState& pmm_state_init_once() noexcept {
    if (!g_pmm) {
        g_pmm = new (g_pmm_storage) PmmState();
    }

    return *g_pmm;
}

} /* namespace kernel */

extern "C" {

/*
 * Keep the C ABI thin.
 * Do policy in C++ and treat these as the ABI boundary only.
 */

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr) {
    kernel::PmmState& pmm = kernel::pmm_state_init_once();

    pmm.init(mem_size, kernel_end_addr);
}

void pmm_init_regions(
    const pmm_region_t* regions, uint32_t region_count,
    const pmm_reserved_region_t* reserved, uint32_t reserved_count,
    uint32_t kernel_end_addr
) {
    kernel::PmmState& pmm = kernel::pmm_state_init_once();

    pmm.init_regions(
        regions, region_count,
        reserved, reserved_count, kernel_end_addr
    );
}

void* pmm_alloc_pages(uint32_t order) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return nullptr;
    }

    return pmm->alloc_pages(order);
}

void* pmm_alloc_pages_zone(uint32_t order, pmm_zone_t zone) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return nullptr;
    }

    return pmm->alloc_pages_zone(order, zone);
}

void pmm_free_pages(void* addr, uint32_t order) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return;
    }

    pmm->free_pages(addr, order);
}

void* pmm_alloc_block(void) {
    return pmm_alloc_pages(0u);
}

void pmm_free_block(void* addr) {
    pmm_free_pages(addr, 0u);
}

static void pmm_free_block_rcu_cb(rcu_head_t* head) {
    if (!head) {
        return;
    }

    page_t* page = container_of(head, page_t, rcu);

    void* phys_addr = (void*)(uintptr_t)kernel::pmm_state()->page_to_phys(page);

    kernel::pmm_state()->free_pages(phys_addr, 0u);
}

void pmm_free_block_deferred(void* addr) {
    if (!addr) {
        return;
    }

    page_t* page = kernel::pmm_state()->phys_to_page(
        (uint32_t)(uintptr_t)addr
    );

    if (!page) {
        pmm_free_block(addr);
        return;
    }

    if ((page->flags & PMM_FLAG_KERNEL) != 0u) {
        pmm_free_block(addr);
        return;
    }

    call_rcu(&page->rcu, pmm_free_block_rcu_cb);
}

page_t* pmm_phys_to_page(uint32_t phys_addr) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return nullptr;
    }

    return pmm->phys_to_page(phys_addr);
}

uint32_t pmm_page_to_phys(page_t* page) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return 0u;
    }

    return pmm->page_to_phys(page);
}

uint32_t pmm_get_used_blocks(void) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return 0u;
    }

    return pmm->get_used_blocks();
}

uint32_t pmm_get_free_blocks(void) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return 0u;
    }

    return pmm->get_free_blocks();
}

uint32_t pmm_get_total_blocks(void) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return 0u;
    }

    return pmm->get_total_blocks();
}

}

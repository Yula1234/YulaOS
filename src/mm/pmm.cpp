/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <lib/compiler.h>

#include <lib/cpp/lock_guard.h>

#include "pmm.h"

#include <kernel/cpu.h>

namespace kernel {

namespace {

struct __cacheline_aligned PerCpuPageCache {
    uint32_t count = 0u;
    void* phys_pages[256]{};
};

static constexpr uint32_t pcp_capacity = 256u;
static constexpr uint32_t pcp_refill_batch = 32u;
static constexpr uint32_t pcp_drain_batch = 128u;

static PerCpuPageCache pcp_caches[PMM_ZONE_COUNT][MAX_CPUS]{};

static inline uint32_t pcp_cpu_index() {
    cpu_t* cpu = cpu_current();
    if (!cpu) {
        return 0u;
    }

    const int idx = cpu->index;
    if (idx < 0 || idx >= MAX_CPUS) {
        return 0u;
    }

    return static_cast<uint32_t>(idx);
}

static inline PerCpuPageCache& pcp_current_cache(pmm_zone_t zone) {
    return pcp_caches[zone][pcp_cpu_index()];
}

static inline void pcp_cache_drain(PmmState* pmm, PerCpuPageCache& cache) {
    if (!pmm || cache.count < pcp_capacity) {
        return;
    }

    void* batch[pcp_drain_batch] = {};
    const uint32_t drain = (cache.count < pcp_drain_batch) ? cache.count : pcp_drain_batch;

    for (uint32_t i = 0u; i < drain; i++) {
        batch[i] = cache.phys_pages[cache.count - 1u - i];
    }

    cache.count -= drain;
    pmm->free_pages_order0_batch(batch, drain);
}

static inline void* pcp_alloc_from_cache(PmmState* pmm, pmm_zone_t zone) {
    kernel::ScopedIrqDisable irq_guard;

    PerCpuPageCache& cache = pcp_current_cache(zone);
    if (cache.count > 0u) {
        return cache.phys_pages[--cache.count];
    }

    void* batch[pcp_refill_batch] = {};
    const uint32_t n = pmm->alloc_pages_order0_batch(zone, batch, pcp_refill_batch);
    if (n == 0u) {
        return nullptr;
    }

    for (uint32_t i = 1u; i < n && cache.count < pcp_capacity; i++) {
        cache.phys_pages[cache.count++] = batch[i];
    }

    return batch[0];
}

static inline void pcp_free_to_cache(PmmState* pmm, pmm_zone_t zone, void* addr) {
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

/*
 * Single instance is enough: PMM is a global resource, and keeping it in a
 * fixed storage avoids boot-time dynamic allocation dependencies.
 */
alignas(PmmState) static unsigned char g_pmm_storage[sizeof(PmmState)];
static PmmState* g_pmm = nullptr;

/*
 * Conventional ISA DMA ceiling.
 * Anything below is treated as the DMA zone for alloc_pages_zone().
 */
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
    const pmm_region_t* regions,
    uint32_t region_count,
    const pmm_reserved_region_t* reserved,
    uint32_t reserved_count,
    uint32_t kernel_end_addr
) noexcept {
    /*
     * Initialization is split in two phases:
     *  1) pessimistically mark every page used
     *  2) free pages that fall into AVAILABLE regions and are not reserved
     *
     * This makes it hard to accidentally hand out memory we never meant to.
     */
    total_pages_ = 0u;
    used_pages_count_ = 0u;
    mem_map_ = nullptr;

    for (uint32_t zone = 0u; zone < PMM_ZONE_COUNT; zone++) {
        for (uint32_t order = 0u; order <= PMM_MAX_ORDER; order++) {
            zones_[zone].free_areas[order] = {};
        }

        zones_[zone].free_bitmap = 0u;
    }

    if (!regions || region_count == 0u) {
        return;
    }

    /*
     * Find the highest available address to size `mem_map_`.
     * We intentionally ignore reserved/non-available regions here.
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

        /* Guard against 32-bit overflow in base+size. */
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
     * The page metadata lives in physical memory right after the kernel.
     * This assumes the kernel has already reserved that range.
     */
    const uint32_t mem_map_phys = align_up(kernel_end_addr);
    mem_map_ = reinterpret_cast<page_t*>(mem_map_phys);

    const uint32_t mem_map_size = total_pages_ * static_cast<uint32_t>(sizeof(page_t));
    const uint32_t mem_map_end = align_up(mem_map_phys + mem_map_size);
    if (mem_map_end < mem_map_phys) {
        mem_map_ = nullptr;
        total_pages_ = 0u;
        return;
    }

    memset(mem_map_, 0, mem_map_size);

    init_used_pages(total_pages_, mem_map_end);

    /*
     * Free all usable ranges.
     * We clamp against mem_map_end so metadata itself is never freed.
     */
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
        if (zone >= PMM_ZONE_COUNT) {
            return nullptr;
        }

        return pcp_alloc_from_cache(this, zone);
    }

    SpinLockSafeGuard guard(zones_[zone].lock);
    return alloc_pages_zone_unlocked(order, zone);
}

void* PmmState::alloc_pages_zone_unlocked(uint32_t order, pmm_zone_t zone) noexcept {
    if (order > PMM_MAX_ORDER) {
        return nullptr;
    }

    if (zone >= PMM_ZONE_COUNT) {
        return nullptr;
    }

    /*
     * Buddy allocation:
     * pick the smallest order that has a free block, then split down to the
     * requested order while putting the spare buddies back on freelists.
     */

    const uint32_t bitmap = zones_[zone].free_bitmap;
    const uint32_t order_mask = (order == 0u) ? 0u : ((1u << order) - 1u);
    const uint32_t available = bitmap & ~order_mask;

    if (available == 0u) {
        return nullptr;
    }

    uint32_t current_order = static_cast<uint32_t>(__builtin_ctz(available));

    page_t* page = free_area_pop(zone, current_order);
    if (!page) {
        return nullptr;
    }

    page->flags |= PMM_FLAG_USED;
    page->flags &= ~PMM_FLAG_FREE;
    page->ref_count = 1;

    page->slab_cache = nullptr;
    page->freelist = nullptr;
    page->objects = 0;
    page->prev = nullptr;
    page->next = nullptr;

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
    __atomic_fetch_add(&used_pages_count_, 1u << order, __ATOMIC_RELAXED);

    return reinterpret_cast<void*>(page_to_phys(page));
}

uint32_t PmmState::alloc_pages_order0_batch(pmm_zone_t preferred, void** out, uint32_t cap) noexcept {
    if (!out || cap == 0u) {
        return 0u;
    }

    if (preferred >= PMM_ZONE_COUNT) {
        preferred = PMM_ZONE_NORMAL;
    }

    pmm_zone_t fallback = PMM_ZONE_NORMAL;
    if (preferred == PMM_ZONE_NORMAL) {
        fallback = PMM_ZONE_DMA;
    }

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
    if (!pages || n == 0u) {
        return;
    }

    void* dma_pages[pcp_drain_batch] = {};
    void* normal_pages[pcp_drain_batch] = {};

    uint32_t dma_count = 0u;
    uint32_t normal_count = 0u;

    for (uint32_t i = 0u; i < n; i++) {
        if (!pages[i]) {
            continue;
        }

        page_t* page = phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pages[i])));
        if (!page) {
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
    if (!addr) {
        return;
    }

    if (order > PMM_MAX_ORDER) {
        return;
    }

    page_t* page = phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)));
    if (!page) {
        return;
    }

    if (order == 0u) {
        if ((page->flags & PMM_FLAG_KERNEL) != 0u) {
            return;
        }

        if ((page->flags & PMM_FLAG_USED) == 0u) {
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

    if (idx >= total_pages_) {
        return nullptr;
    }

    return &mem_map_[idx];
}

uint32_t PmmState::page_to_phys(page_t* page) const noexcept {
    const uint32_t idx = static_cast<uint32_t>(page - mem_map_);

    return idx * PAGE_SIZE;
}

uint32_t PmmState::get_used_blocks() const noexcept {
    return __atomic_load_n(&used_pages_count_, __ATOMIC_RELAXED);
}

uint32_t PmmState::get_free_blocks() const noexcept {
    return total_pages_ - __atomic_load_n(&used_pages_count_, __ATOMIC_RELAXED);
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
    /*
     * Start with everything marked used.
     * Later we selectively free the ranges we're willing to allocate from.
     */
    used_pages_count_ = total_pages;

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
        page.prev = nullptr;
        page.next = nullptr;
    }
}

void PmmState::free_range(
    uint32_t start,
    uint32_t end,
    uint32_t zone_flags,
    const pmm_reserved_region_t* reserved,
    uint32_t reserved_count
) noexcept {
    if (end <= start) {
        return;
    }

    uint32_t cur = align_up(start);
    const uint32_t end_aligned = align_down(end);
    if (end_aligned <= cur) {
        return;
    }

    if (zone_flags != 0u && (zone_flags & PMM_FLAG_DMA) == 0u && cur < k_dma_limit) {
        cur = k_dma_limit;
    }

    /*
     * Free the range using the largest blocks that fit and do not cross zone
     * boundaries. This builds initial freelists in near-optimal shape.
     */
    while (cur < end_aligned) {
        if (zone_flags != 0u && (zone_flags & PMM_FLAG_DMA) != 0u && cur >= k_dma_limit) {
            break;
        }

        uint32_t max_order = PMM_MAX_ORDER;
        while (max_order > 0u) {
            const uint32_t block_size = PAGE_SIZE << max_order;
            const uint32_t block_end = cur + block_size;

            if (block_end < cur || block_end > end_aligned) {
                max_order--;
                continue;
            }

            const uint32_t cur_zone_flags = zone_flags != 0u ? zone_flags : zone_flags_for_addr(cur);
            if ((cur_zone_flags & PMM_FLAG_DMA) != 0u) {
                if (block_end > k_dma_limit) {
                    max_order--;
                    continue;
                }
            } else if (cur < k_dma_limit && block_end > k_dma_limit) {
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

        if (block_end <= cur || block_end > end_aligned) {
            break;
        }

        if (range_is_reserved(cur, block_end, reserved, reserved_count)) {
            cur += PAGE_SIZE;
            continue;
        }

        free_pages_unlocked(reinterpret_cast<void*>(cur), max_order);
        cur = block_end;
    }
}

bool PmmState::range_is_reserved(
    uint32_t start,
    uint32_t end,
    const pmm_reserved_region_t* reserved,
    uint32_t reserved_count
) const noexcept {
    if (!reserved || reserved_count == 0u) {
        return false;
    }

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
    page->next = *head;
    page->prev = nullptr;

    if (*head) {
        (*head)->prev = page;
    }

    *head = page;
}

void PmmState::list_remove(page_t** head, page_t* page) noexcept {
    if (page->prev) {
        page->prev->next = page->next;
    } else {
        *head = page->next;
    }

    if (page->next) {
        page->next->prev = page->prev;
    }

    page->next = nullptr;
    page->prev = nullptr;
}

void PmmState::free_area_push(pmm_zone_t zone, uint32_t order, page_t* page) noexcept {
    FreeArea& area = zones_[zone].free_areas[order];

    const uint32_t prev_count = area.count;
    list_add(&area.head, page);
    area.count = prev_count + 1u;

    if (prev_count == 0u) {
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
        zones_[zone].free_bitmap &= ~(1u << order);
    }

    return page;
}

void PmmState::free_area_remove(pmm_zone_t zone, uint32_t order, page_t* page) noexcept {
    FreeArea& area = zones_[zone].free_areas[order];
    list_remove(&area.head, page);
    area.count--;

    if (area.count == 0u) {
        zones_[zone].free_bitmap &= ~(1u << order);
    }
}

void PmmState::free_pages_unlocked(void* addr, uint32_t order) noexcept {
    page_t* page = phys_to_page(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr)));
    if (!page) {
        return;
    }

    /*
     * We silently ignore invalid frees.
     * At this layer we don't have a general-purpose reporting channel.
     */
    if ((page->flags & PMM_FLAG_USED) == 0u) {
        return;
    }

    /*
     * Kernel image and PMM metadata are pinned for the whole system lifetime.
     */
    if ((page->flags & PMM_FLAG_KERNEL) != 0u) {
        return;
    }

    __atomic_fetch_sub(&used_pages_count_, 1u << order, __ATOMIC_RELAXED);

    uint32_t pfn = static_cast<uint32_t>(page - mem_map_);
    const pmm_zone_t zone = zone_for_flags(page->flags);

    /*
     * Buddy merge:
     * repeatedly check if the buddy is free and of the same order and zone.
     * If yes, remove it from freelist and grow the block.
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
    page->prev = nullptr;
    page->next = nullptr;

    page->flags = (page->flags | PMM_FLAG_FREE) & ~PMM_FLAG_USED;
    page->order = order;
    page->ref_count = 0;

    free_area_push(zone, order, page);
}

static inline PmmState& pmm_state_init_once() noexcept {
    /*
     * Construct on first use.
     * Boot code is expected to call init once; this is defensive.
     */
    if (!g_pmm) {
        g_pmm = new (g_pmm_storage) PmmState();
    }

    return *g_pmm;
}

} /* namespace kernel */

extern "C" {

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr) {
    kernel::PmmState& pmm = kernel::pmm_state_init_once();

    pmm.init(mem_size, kernel_end_addr);
}

void pmm_init_regions(
    const pmm_region_t* regions,
    uint32_t region_count,
    const pmm_reserved_region_t* reserved,
    uint32_t reserved_count,
    uint32_t kernel_end_addr
) {
    kernel::PmmState& pmm = kernel::pmm_state_init_once();

    pmm.init_regions(regions, region_count, reserved, reserved_count, kernel_end_addr);
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

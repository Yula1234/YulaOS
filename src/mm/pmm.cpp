/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <lib/compiler.h>

#include "pmm.h"

namespace kernel {

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
            free_areas_[zone][order] = {};
        }
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
    void* ptr = alloc_pages_zone(order, PMM_ZONE_NORMAL);
    if (ptr) {
        return ptr;
    }

    return alloc_pages_zone(order, PMM_ZONE_DMA);
}

void* PmmState::alloc_pages_zone(uint32_t order, pmm_zone_t zone) noexcept {
    if (order > PMM_MAX_ORDER) {
        return nullptr;
    }

    if (zone >= PMM_ZONE_COUNT) {
        return nullptr;
    }

    SpinLockSafeGuard guard(lock_);

    /*
     * Buddy allocation:
     * pick the smallest order that has a free block, then split down to the
     * requested order while putting the spare buddies back on freelists.
     */

    uint32_t current_order = order;
    while (current_order <= PMM_MAX_ORDER) {
        if (free_areas_[zone][current_order].head) {
            break;
        }

        current_order++;
    }

    if (current_order > PMM_MAX_ORDER) {
        return nullptr;
    }

    page_t* page = free_areas_[zone][current_order].head;
    list_remove(&free_areas_[zone][current_order].head, page);
    free_areas_[zone][current_order].count--;

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

        list_add(&free_areas_[zone][current_order].head, buddy);
        free_areas_[zone][current_order].count++;
    }

    page->order = order;
    used_pages_count_ += 1u << order;

    return reinterpret_cast<void*>(page_to_phys(page));
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

    SpinLockSafeGuard guard(lock_);

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
    return used_pages_count_;
}

uint32_t PmmState::get_free_blocks() const noexcept {
    return total_pages_ - used_pages_count_;
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

    used_pages_count_ -= 1u << order;

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

        list_remove(&free_areas_[zone][order].head, buddy);
        free_areas_[zone][order].count--;

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

    list_add(&free_areas_[zone][order].head, page);
    free_areas_[zone][order].count++;
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

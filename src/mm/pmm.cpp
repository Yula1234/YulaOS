// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <lib/compiler.h>

#include "pmm.h"

namespace kernel {

alignas(PmmState) static unsigned char g_pmm_storage[sizeof(PmmState)];
static PmmState* g_pmm = nullptr;

PmmState* pmm_state() noexcept {
    return g_pmm;
}

void PmmState::init(uint32_t mem_size, uint32_t kernel_end_addr) noexcept {
    total_pages_ = mem_size / PAGE_SIZE;
    used_pages_count_ = 0u;

    for (uint32_t i = 0; i <= PMM_MAX_ORDER; i++) {
        free_areas_[i] = {};
    }

    const uint32_t mem_map_phys = align_up(kernel_end_addr);
    mem_map_ = reinterpret_cast<page_t*>(mem_map_phys);

    const uint32_t mem_map_size = total_pages_ * static_cast<uint32_t>(sizeof(page_t));
    memset(mem_map_, 0, mem_map_size);

    const uint32_t phys_alloc_start = align_up(mem_map_phys + mem_map_size);
    const uint32_t first_free_idx = phys_alloc_start / PAGE_SIZE;

    used_pages_count_ = total_pages_;

    for (uint32_t p = first_free_idx; p < total_pages_; p++) {
        mem_map_[p].flags = PMM_FLAG_USED;
        mem_map_[p].ref_count = 0;
        mem_map_[p].order = 0;
    }

    for (uint32_t i = 0; i < first_free_idx; i++) {
        mem_map_[i].flags = PMM_FLAG_USED | PMM_FLAG_KERNEL;
        mem_map_[i].ref_count = 1;
        mem_map_[i].order = 0;
    }

    uint32_t i = first_free_idx;
    const uint32_t max_block_size = 1u << PMM_MAX_ORDER;

    while (i < total_pages_ && (i & (max_block_size - 1u)) != 0u) {
        mem_map_[i].flags = PMM_FLAG_USED;

        free_pages_unlocked(reinterpret_cast<void*>(i * PAGE_SIZE), 0u);

        i++;
    }

    while (i + max_block_size <= total_pages_) {
        page_t* page = &mem_map_[i];

        for (uint32_t j = 0; j < max_block_size; j++) {
            mem_map_[i + j].flags = PMM_FLAG_USED;
            mem_map_[i + j].order = 0;
            mem_map_[i + j].ref_count = 0;
        }

        page->flags = PMM_FLAG_FREE;
        page->order = PMM_MAX_ORDER;
        page->ref_count = 0;

        list_add(&free_areas_[PMM_MAX_ORDER].head, page);
        free_areas_[PMM_MAX_ORDER].count++;

        used_pages_count_ -= max_block_size;

        i += max_block_size;
    }

    while (i < total_pages_) {
        mem_map_[i].flags = PMM_FLAG_USED;

        free_pages_unlocked(reinterpret_cast<void*>(i * PAGE_SIZE), 0u);

        i++;
    }
}

void* PmmState::alloc_pages(uint32_t order) noexcept {
    if (order > PMM_MAX_ORDER) {
        return nullptr;
    }

    SpinLockSafeGuard guard(lock_);

    uint32_t current_order = order;
    while (current_order <= PMM_MAX_ORDER) {
        if (free_areas_[current_order].head) {
            break;
        }

        current_order++;
    }

    if (current_order > PMM_MAX_ORDER) {
        return nullptr;
    }

    page_t* page = free_areas_[current_order].head;
    list_remove(&free_areas_[current_order].head, page);
    free_areas_[current_order].count--;

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

        buddy->flags = PMM_FLAG_FREE;
        buddy->order = current_order;
        buddy->ref_count = 0;

        list_add(&free_areas_[current_order].head, buddy);
        free_areas_[current_order].count++;
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

    if ((page->flags & PMM_FLAG_USED) == 0u) {
        return;
    }

    used_pages_count_ -= 1u << order;

    uint32_t pfn = static_cast<uint32_t>(page - mem_map_);

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

        list_remove(&free_areas_[order].head, buddy);
        free_areas_[order].count--;

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

    page->flags = PMM_FLAG_FREE;
    page->order = order;
    page->ref_count = 0;

    list_add(&free_areas_[order].head, page);
    free_areas_[order].count++;
}

static inline PmmState& pmm_state_init_once() noexcept {
    if (!g_pmm) {
        g_pmm = new (g_pmm_storage) PmmState();
    }

    return *g_pmm;
}

} // namespace kernel

extern "C" {

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr) {
    kernel::PmmState& pmm = kernel::pmm_state_init_once();

    pmm.init(mem_size, kernel_end_addr);
}

void* pmm_alloc_pages(uint32_t order) {
    kernel::PmmState* pmm = kernel::pmm_state();

    if (kernel::unlikely(!pmm)) {
        return nullptr;
    }

    return pmm->alloc_pages(order);
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

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <lib/compiler.h>

#include "pmm.h"

#include <kernel/boot.h>

namespace kernel {

static inline uint32_t align_down_4k_u32(uint32_t v) noexcept {
    return v & ~0xFFFu;
}

static inline uint32_t align_up_4k_u32(uint32_t v) noexcept {
    if ((v & 0xFFFu) == 0u) {
        return v;
    }

    return (v & ~0xFFFu) + 0x1000u;
}

static inline uint32_t clamp_end_u32(uint64_t end) noexcept {
    if (end > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;
    }

    return (uint32_t)end;
}

static uint32_t multiboot_detect_max_usable_end(const multiboot_info_t* mb_info) noexcept {
    uint64_t memory_end_addr64 = 0;

    constexpr uint64_t k_low_4g_excl = 0x100000000ull;

    if ((mb_info->flags & (1u << 6)) != 0u) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;

        uint32_t off = 0;
        while (off + sizeof(uint32_t) <= mmap_len) {
            multiboot_memory_map_t* e = (multiboot_memory_map_t*)(mmap_base + off);
            if (e->size == 0) {
                break;
            }

            const uint32_t step = e->size + sizeof(uint32_t);
            if (step > mmap_len - off) {
                break;
            }

            if (e->type == 1) {
                const uint64_t start = e->addr;

                uint64_t end = start + e->len;
                if (end < start) {
                    end = k_low_4g_excl;
                }

                if (start < k_low_4g_excl) {
                    if (end > k_low_4g_excl) {
                        end = k_low_4g_excl;
                    }

                    if (end > memory_end_addr64) {
                        memory_end_addr64 = end;
                    }
                }
            }

            off += step;
        }
    } else if ((mb_info->flags & (1u << 0)) != 0u) {
        memory_end_addr64 = (uint64_t)(mb_info->mem_upper * 1024u) + 0x100000ull;
    }

    if (memory_end_addr64 == 0) {
        memory_end_addr64 = 1024ull * 1024ull * 64ull;
    }

    return clamp_end_u32(memory_end_addr64);
}

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

void PmmState::init_multiboot(const struct multiboot_info* mb_info_opaque, uint32_t kernel_end_addr) noexcept {
    const multiboot_info_t* mb_info = (const multiboot_info_t*)mb_info_opaque;
    const uint32_t mem_end = multiboot_detect_max_usable_end(mb_info);

    constexpr uint64_t k_low_4g_excl = 0x100000000ull;

    total_pages_ = mem_end / PAGE_SIZE;
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

    for (uint32_t p = 0; p < total_pages_; p++) {
        mem_map_[p].flags = PMM_FLAG_USED;
        mem_map_[p].ref_count = 1;
        mem_map_[p].order = 0;
    }

    auto is_reserved_page = [&](uint32_t phys_addr) noexcept {
        if (phys_addr < phys_alloc_start) {
            return true;
        }

        uint32_t mb_info_addr = (uint32_t)(uintptr_t)mb_info;
        if (phys_addr >= align_down_4k_u32(mb_info_addr) && phys_addr < align_up_4k_u32(mb_info_addr + (uint32_t)sizeof(*mb_info))) {
            return true;
        }

        if ((mb_info->flags & (1u << 6)) != 0u) {
            if (phys_addr >= align_down_4k_u32(mb_info->mmap_addr) && phys_addr < align_up_4k_u32(mb_info->mmap_addr + mb_info->mmap_length)) {
                return true;
            }
        }

        if ((mb_info->flags & (1u << 12)) != 0u) {
            uint64_t fb_size64 = (uint64_t)mb_info->framebuffer_pitch * (uint64_t)mb_info->framebuffer_height;
            if (fb_size64 != 0 && fb_size64 <= 0xFFFFFFFFull) {
                uint32_t fb_base = (uint32_t)mb_info->framebuffer_addr;
                uint32_t fb_size = (uint32_t)fb_size64;

                if (fb_base + fb_size >= fb_base) {
                    if (phys_addr >= align_down_4k_u32(fb_base) && phys_addr < align_up_4k_u32(fb_base + fb_size)) {
                        return true;
                    }
                }
            }
        }

        return false;
    };

    const uint32_t pmm_end = total_pages_ * PAGE_SIZE;

    if ((mb_info->flags & (1u << 6)) != 0u) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;

        uint32_t off = 0;
        while (off + sizeof(uint32_t) <= mmap_len) {
            multiboot_memory_map_t* e = (multiboot_memory_map_t*)(mmap_base + off);
            if (e->size == 0) {
                break;
            }

            uint32_t step = e->size + sizeof(uint32_t);
            if (step > mmap_len - off) {
                break;
            }

            off += step;

            if (e->type != 1) {
                continue;
            }

            uint64_t start64 = e->addr;
            uint64_t end64 = start64 + e->len;
            if (end64 < start64) {
                end64 = k_low_4g_excl;
            }

            if (start64 >= k_low_4g_excl) {
                continue;
            }

            if (end64 > k_low_4g_excl) {
                end64 = k_low_4g_excl;
            }

            uint32_t start = (uint32_t)start64;
            uint32_t end_excl = (uint32_t)end64;
            if (end_excl > pmm_end) {
                end_excl = pmm_end;
            }

            start = align_up_4k_u32(start);
            end_excl = align_down_4k_u32(end_excl);

            for (uint32_t addr = start; addr < end_excl; addr += PAGE_SIZE) {
                if (is_reserved_page(addr)) {
                    continue;
                }

                if (addr / PAGE_SIZE >= total_pages_) {
                    break;
                }

                page_t* page = &mem_map_[addr / PAGE_SIZE];
                page->flags = PMM_FLAG_USED;
                page->ref_count = 0;
                page->order = 0;

                free_pages_unlocked((void*)addr, 0u);
            }
        }
    } else {
        uint32_t start = align_up_4k_u32(phys_alloc_start);
        uint32_t end_excl = align_down_4k_u32(mem_end);

        if (end_excl > pmm_end) {
            end_excl = pmm_end;
        }

        for (uint32_t addr = start; addr < end_excl; addr += PAGE_SIZE) {
            if (is_reserved_page(addr)) {
                continue;
            }

            page_t* page = &mem_map_[addr / PAGE_SIZE];
            page->flags = PMM_FLAG_USED;
            page->ref_count = 0;
            page->order = 0;

            free_pages_unlocked((void*)addr, 0u);
        }
    }

    for (uint32_t i = 0; i < first_free_idx; i++) {
        mem_map_[i].flags = PMM_FLAG_USED | PMM_FLAG_KERNEL;
        mem_map_[i].ref_count = 1;
        mem_map_[i].order = 0;
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

void pmm_init_multiboot(const struct multiboot_info* mb_info, uint32_t kernel_end_addr) {
    kernel::PmmState& pmm = kernel::pmm_state_init_once();

    pmm.init_multiboot(mb_info, kernel_end_addr);
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

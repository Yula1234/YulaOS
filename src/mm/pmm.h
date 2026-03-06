// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_ALIGN(x)   (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))

#define PMM_MAX_ORDER   11

typedef enum {
    PMM_FLAG_FREE     = 0,
    PMM_FLAG_USED     = (1 << 0),
    PMM_FLAG_KERNEL   = (1 << 1),
    PMM_FLAG_DMA      = (1 << 2),
} page_flags_t;

typedef enum {
    PMM_ZONE_DMA = 0,
    PMM_ZONE_NORMAL = 1,
    PMM_ZONE_COUNT = 2,
} pmm_zone_t;

typedef enum {
    PMM_REGION_AVAILABLE = 1,
    PMM_REGION_RESERVED = 2,
} pmm_region_type_t;

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t type;
} pmm_region_t;

typedef struct {
    uint32_t base;
    uint32_t size;
} pmm_reserved_region_t;

typedef struct page {
    uint32_t flags;
    int32_t  ref_count;
    uint32_t order;

    void* slab_cache;
    void* freelist;
    uint16_t objects;

    struct page* prev;
    struct page* next;
} page_t;

#ifdef __cplusplus

#include <lib/cpp/lock_guard.h>

namespace kernel {

class PmmState {
public:
    void init(uint32_t mem_size, uint32_t kernel_end_addr) noexcept;
    void init_regions(
        const pmm_region_t* regions,
        uint32_t region_count,
        const pmm_reserved_region_t* reserved,
        uint32_t reserved_count,
        uint32_t kernel_end_addr
    ) noexcept;

    [[nodiscard]] void* alloc_pages(uint32_t order) noexcept;
    [[nodiscard]] void* alloc_pages_zone(uint32_t order, pmm_zone_t zone) noexcept;
    void free_pages(void* addr, uint32_t order) noexcept;

    [[nodiscard]] page_t* phys_to_page(uint32_t phys_addr) noexcept;

    [[nodiscard]] uint32_t page_to_phys(page_t* page) const noexcept;

    [[nodiscard]] uint32_t get_used_blocks() const noexcept;
    [[nodiscard]] uint32_t get_free_blocks() const noexcept;
    [[nodiscard]] uint32_t get_total_blocks() const noexcept;

private:
    static uint32_t align_up(uint32_t addr) noexcept;
    static uint32_t align_down(uint32_t addr) noexcept;
    static void list_add(page_t** head, page_t* page) noexcept;
    static void list_remove(page_t** head, page_t* page) noexcept;

    void free_pages_unlocked(void* addr, uint32_t order) noexcept;
    void init_used_pages(uint32_t total_pages, uint32_t kernel_end_addr) noexcept;
    void free_range(uint32_t start, uint32_t end, uint32_t zone_flags, const pmm_reserved_region_t* reserved, uint32_t reserved_count) noexcept;
    bool range_is_reserved(uint32_t start, uint32_t end, const pmm_reserved_region_t* reserved, uint32_t reserved_count) const noexcept;
    static uint32_t zone_flags_for_addr(uint32_t addr) noexcept;
    static pmm_zone_t zone_for_flags(uint32_t flags) noexcept;

    SpinLock lock_;

    page_t* mem_map_ = nullptr;
    uint32_t total_pages_ = 0u;
    uint32_t used_pages_count_ = 0u;

    struct FreeArea {
        page_t* head = nullptr;
        uint32_t count = 0u;
    };

    FreeArea free_areas_[PMM_ZONE_COUNT][PMM_MAX_ORDER + 1]{};
};

[[nodiscard]] PmmState* pmm_state() noexcept;

} // namespace kernel

#endif

#ifdef __cplusplus
extern "C" {
#endif

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr);
void pmm_init_regions(
    const pmm_region_t* regions,
    uint32_t region_count,
    const pmm_reserved_region_t* reserved,
    uint32_t reserved_count,
    uint32_t kernel_end_addr
);

void* pmm_alloc_block(void);
void pmm_free_block(void* addr);

void* pmm_alloc_pages(uint32_t order);
void* pmm_alloc_pages_zone(uint32_t order, pmm_zone_t zone);
void pmm_free_pages(void* addr, uint32_t order);

page_t* pmm_phys_to_page(uint32_t phys_addr);
uint32_t pmm_page_to_phys(page_t* page);

uint32_t pmm_get_used_blocks(void);
uint32_t pmm_get_free_blocks(void);
uint32_t pmm_get_total_blocks(void);

#ifdef __cplusplus
}
#endif

#endif

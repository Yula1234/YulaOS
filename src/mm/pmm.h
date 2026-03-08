/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>
#include <stddef.h>

/*
 * Physical Memory Manager.
 *
 * This is a page-based allocator. The core implementation is a buddy system
 * with per-zone freelists.
 *
 * The API returns physical addresses. They are carried as `void*` to keep the
 * call sites lightweight, but the value is not a virtual pointer.
 */

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_ALIGN(x)   (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))

/*
 * Maximum buddy order. `order == n` means a block size of
 *   (PAGE_SIZE << n)
 * and (1 << n) base pages.
 */
#define PMM_MAX_ORDER   11

typedef enum {
    PMM_FLAG_FREE     = 0,
    PMM_FLAG_USED     = (1 << 0),
    PMM_FLAG_KERNEL   = (1 << 1),
    PMM_FLAG_DMA      = (1 << 2),
} page_flags_t;

/*
 * Allocation zones.
 *
 * DMA is intended for low memory used by devices with addressing limits.
 * NORMAL is everything else.
 */
typedef enum {
    PMM_ZONE_DMA = 0,
    PMM_ZONE_NORMAL = 1,
    PMM_ZONE_COUNT = 2,
} pmm_zone_t;

typedef enum {
    PMM_REGION_AVAILABLE = 1,
    PMM_REGION_RESERVED = 2,
} pmm_region_type_t;

/*
 * Memory map as provided by early boot code.
 *
 * base/size are physical byte ranges.
 * type is a pmm_region_type_t value.
 */
typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t type;
} pmm_region_t;

/*
 * Extra "hands-off" ranges inside otherwise available memory.
 *
 * Use this for things like ACPI tables, MMIO windows, bootloader scratch
 * buffers, etc.
 */
typedef struct {
    uint32_t base;
    uint32_t size;
} pmm_reserved_region_t;

/*
 * Per-page metadata.
 *
 * This structure is also used as the node for buddy freelists.
 * When a page is allocated, list pointers must not be relied upon.
 */
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
    /*
     * Initialize PMM with a single [0, mem_size) available range.
     * `kernel_end_addr` is a physical address and is treated as the end of the
     * kernel image. Internal metadata is placed after it.
     */
    void init(uint32_t mem_size, uint32_t kernel_end_addr) noexcept;

    /*
     * Initialize PMM from an explicit region list plus optional reservations.
     *
     * Only PMM_REGION_AVAILABLE ranges are considered.
     * `reserved` further carves out subranges from those.
     */
    void init_regions(
        const pmm_region_t* regions,
        uint32_t region_count,
        const pmm_reserved_region_t* reserved,
        uint32_t reserved_count,
        uint32_t kernel_end_addr
    ) noexcept;

    /*
     * Allocate 2^order contiguous pages.
     *
     * Returns a physical base address, aligned to (PAGE_SIZE << order), or
     * nullptr on failure.
     */
    [[nodiscard]] void* alloc_pages(uint32_t order) noexcept;

    /*
     * Same as alloc_pages(), but restricted to a specific zone.
     */
    [[nodiscard]] void* alloc_pages_zone(uint32_t order, pmm_zone_t zone) noexcept;

    /*
     * Free a block previously allocated with alloc_pages*().
     * `addr` must be the same physical base and `order` must match.
     */
    void free_pages(void* addr, uint32_t order) noexcept;

    /*
     * Translate a physical address to per-page metadata.
     * The address does not need to be page-aligned.
     */
    [[nodiscard]] page_t* phys_to_page(uint32_t phys_addr) noexcept;

    /*
     * Return base physical address of the page described by `page`.
     */
    [[nodiscard]] uint32_t page_to_phys(page_t* page) const noexcept;

    [[nodiscard]] uint32_t get_used_blocks() const noexcept;
    [[nodiscard]] uint32_t get_free_blocks() const noexcept;
    [[nodiscard]] uint32_t get_total_blocks() const noexcept;

private:
    static uint32_t align_up(uint32_t addr) noexcept;
    static uint32_t align_down(uint32_t addr) noexcept;

    static void list_add(page_t** head, page_t* page) noexcept;
    static void list_remove(page_t** head, page_t* page) noexcept;

    void free_area_push(pmm_zone_t zone, uint32_t order, page_t* page) noexcept;
    [[nodiscard]] page_t* free_area_pop(pmm_zone_t zone, uint32_t order) noexcept;
    void free_area_remove(pmm_zone_t zone, uint32_t order, page_t* page) noexcept;

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

    uint32_t free_bitmap_[PMM_ZONE_COUNT]{};
};

/* Global PMM singleton state, created on first init call. */
[[nodiscard]] PmmState* pmm_state() noexcept;

} /* namespace kernel */

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

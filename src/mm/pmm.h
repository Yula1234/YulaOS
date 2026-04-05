/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

/*
 * Physical Memory Manager.
 *
 * Expose a simple C ABI returning physical addresses (carried as void*).
 * Keep the core implementation in C++ to enforce invariants and keep locking
 * structured.
 */

#ifndef MM_PMM_H
#define MM_PMM_H

#include <kernel/rcu.h>

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define PAGE_ALIGN(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))

/*
 * Keep the buddy order small to cap metadata and scanning. Higher orders are
 * still available through the buddy itself by coalescing.
 */
#define PMM_MAX_ORDER 11

typedef enum {
    PMM_FLAG_FREE     = 0,
    PMM_FLAG_USED     = (1u << 0),
    PMM_FLAG_KERNEL   = (1u << 1),
    PMM_FLAG_DMA      = (1u << 2),
} page_flags_t;

/*
 * Split low DMA-addressable memory from the rest.
 * This is a contention win: independent zones can make progress in parallel.
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

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t type;
} pmm_region_t;

typedef struct {
    uint32_t base;
    uint32_t size;
} pmm_reserved_region_t;

/*
 * Keep this struct compact and cache-friendly: the allocator walks it a lot.
 *
 * `order` is buddy-only state.
 * `slab_cache` is SLUB-only state.
 * Those lifetimes do not overlap for an allocated page, so share the storage.
 */
typedef struct page {
    uint32_t flags;
    int32_t  ref_count;

    union {
        uint32_t order;
        void* slab_cache;
    };

    void* freelist;
    uint16_t objects;

    union {
        struct {
            struct page* prev;
            struct page* next;
        } list;
        rcu_head_t rcu;
    };

    uint32_t _reserved;
} page_t;

/*
 * Keep the size a power of two to make page-indexing and cacheline packing
 * predictable. This is a hot structure.
 */
#ifdef __cplusplus
static_assert(sizeof(page_t) == 32u);
#else
typedef char pmm_page_t_size_assert[(sizeof(page_t) == 32u) ? 1 : -1];
#endif

#ifdef __cplusplus

#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>

namespace kernel {

class PmmState {
public:
    /*
     * `kernel_end_addr` pins the kernel image and places PMM metadata after it.
     * Do not rely on dynamic allocations during boot.
     */
    void init(uint32_t mem_size, uint32_t kernel_end_addr) noexcept;

    /*
     * Build allocator state from an explicit memory map.
     * Treat non-AVAILABLE regions and `reserved` subranges as hands-off.
     */
    void init_regions(
        const pmm_region_t* regions, uint32_t region_count,
        const pmm_reserved_region_t* reserved, uint32_t reserved_count,
        uint32_t kernel_end_addr
    ) noexcept;

    /*
     * Return a physical base address aligned to the allocation size.
     * Keep the interface minimal: higher layers pick policies.
     */
    [[nodiscard]] void* alloc_pages(uint32_t order) noexcept;
    [[nodiscard]] void* alloc_pages_zone(uint32_t order, pmm_zone_t zone) noexcept;

    [[nodiscard]] uint32_t alloc_pages_order0_batch(
        pmm_zone_t preferred,
        void** out, uint32_t cap
    ) noexcept;

    void free_pages_order0_batch(void* const* pages, uint32_t n) noexcept;

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

    [[nodiscard]] void* alloc_pages_zone_unlocked(uint32_t order, pmm_zone_t zone) noexcept;

    void free_area_push(pmm_zone_t zone, uint32_t order, page_t* page) noexcept;
    [[nodiscard]] page_t* free_area_pop(pmm_zone_t zone, uint32_t order) noexcept;
    void free_area_remove(pmm_zone_t zone, uint32_t order, page_t* page) noexcept;

    void free_pages_unlocked(void* addr, uint32_t order) noexcept;

    void init_used_pages(uint32_t total_pages, uint32_t kernel_end_addr) noexcept;

    void free_range(
        uint32_t start, uint32_t end,
        uint32_t zone_flags,
        const pmm_reserved_region_t* reserved, uint32_t reserved_count
    ) noexcept;

    bool range_is_reserved(
        uint32_t start, uint32_t end,
        const pmm_reserved_region_t* reserved, uint32_t reserved_count
    ) const noexcept;

    static uint32_t zone_flags_for_addr(uint32_t addr) noexcept;
    static pmm_zone_t zone_for_flags(uint32_t flags) noexcept;

    page_t* mem_map_ = nullptr;
    uint32_t total_pages_ = 0u;

    __cacheline_aligned uint32_t cpu_used_pages_[MAX_CPUS]{};

    struct FreeArea {
        page_t* head = nullptr;
        uint32_t count = 0u;
    };

    struct PmmZone {
        /*
         * Guard the freelists and the bitmap together.
         * Keep the critical section small and the ownership obvious.
         */
        SpinLock lock;
        FreeArea free_areas[PMM_MAX_ORDER + 1]{};
        uint32_t free_bitmap = 0u;
    };

    PmmZone zones_[PMM_ZONE_COUNT]{};
};

[[nodiscard]] PmmState* pmm_state() noexcept;

} /* namespace kernel */

#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Keep the ABI surface stable.
 * Route everything through the C++ singleton to avoid duplicated state.
 */

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr);
void pmm_init_regions(
    const pmm_region_t* regions, uint32_t region_count,
    const pmm_reserved_region_t* reserved, uint32_t reserved_count,
    uint32_t kernel_end_addr
);

void* pmm_alloc_block(void);
void pmm_free_block(void* addr);
void pmm_free_block_deferred(void* addr);

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

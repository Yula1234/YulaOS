/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>
#include <stddef.h>

/*
 * Virtual Memory Manager (kernel heap address-space allocator).
 *
 * This module manages a virtual address range and backs it with physical pages
 * from PMM on demand.
 *
 * alloc_pages() returns a virtual base address; free_pages() unmaps and returns
 * the physical pages back to PMM.
 */

#define KERNEL_HEAP_START 0xC0000000u
#define KERNEL_HEAP_SIZE  0x40000000u
#define PAGE_SIZE         4096

#ifdef __cplusplus

#include <lib/rbtree.h>

#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>

#include <kernel/smp/cpu_limits.h>

namespace kernel {

class PmmState;

/*
 * A free region inside the managed virtual range.
 *
 * Augmented Red-Black Tree: sorted by address for adjacency checks,
 * with subtree_max_size enabling O(log N) best-fit search.
 */
struct VmFreeBlock {
    struct rb_node node;

    uintptr_t start;
    size_t size;
    size_t subtree_max_size;

    VmFreeBlock* next_free;
};

class VmmState {
public:
    /*
     * Initialize kernel heap allocator state.
     * Expects PMM and paging to be ready.
     */
    void init() noexcept;

    /*
     * Allocate `count` pages of virtual space and map fresh physical pages.
     * Returns a virtual base address or nullptr on failure.
     */
    [[nodiscard]] void* alloc_pages(size_t count) noexcept;

    /*
     * Free/unmap `count` pages previously returned by alloc_pages().
     * `ptr` must match the original base address.
     */
    void free_pages(void* ptr, size_t count) noexcept;

    [[nodiscard]] void* reserve_pages(size_t count) noexcept;
    void unreserve_pages(void* ptr, size_t count) noexcept;

    /*
     * Map a single page into the kernel page directory.
     * Returns 1 on success, 0 on invalid alignment.
     */
    [[nodiscard]] int map_page(uint32_t virt, uint32_t phys, uint32_t flags) noexcept;

    /* Number of pages currently allocated from the kernel heap range. */
    [[nodiscard]] size_t get_used_pages() const noexcept;

private:
    static constexpr uint32_t k_max_nodes = 4096u;

    struct __cacheline_aligned PerCpuVmmCache {
        static constexpr size_t k_capacity = 64u;

        uintptr_t free_pages[k_capacity]{};
        size_t count = 0u;
    };

    SpinLock lock_;

    PmmState* pmm_ = nullptr;

    VmFreeBlock node_pool_[k_max_nodes]{};
    VmFreeBlock* free_nodes_head_ = nullptr;

    struct rb_root free_tree_ = RB_ROOT;

    atomic<size_t> used_pages_count_{0u};

    PerCpuVmmCache pcp_caches_[MAX_CPUS]{};
};

[[nodiscard]] VmmState* vmm_state() noexcept;

} /* namespace kernel */

#endif

#ifdef __cplusplus
extern "C" {
#endif

void vmm_init(void);

void* vmm_alloc_pages(size_t pages);
void vmm_free_pages(void* virt, size_t pages);

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

void* vmm_reserve_pages(size_t pages);
void vmm_unreserve_pages(void* virt, size_t pages);

size_t vmm_get_used_pages(void);

#ifdef __cplusplus
}
#endif

#endif
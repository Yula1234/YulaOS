// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>
#include <stddef.h>

#define KERNEL_HEAP_START 0xC0000000u
#define KERNEL_HEAP_SIZE  0x40000000u
#define PAGE_SIZE         4096

#ifdef __cplusplus

#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/rbtree.h>

namespace kernel {

struct VmFreeBlock {
    rb_node node_addr;
    rb_node node_size;

    uintptr_t start;
    size_t size;

    VmFreeBlock* next_free;
};

struct VmFreeBlockAddrKeyOfValue {
    const uintptr_t& operator()(const VmFreeBlock& block) const noexcept;
};

struct VmFreeBlockSizeKey {
    size_t size;
    uintptr_t start;
};

struct VmFreeBlockSizeKeyOfValue {
    VmFreeBlockSizeKey operator()(const VmFreeBlock& block) const noexcept;
};

struct VmFreeBlockSizeKeyCompare {
    bool operator()(const VmFreeBlockSizeKey& a, const VmFreeBlockSizeKey& b) const noexcept;
};

using VmmAddrTree = IntrusiveRbTree<
    VmFreeBlock,
    detail::RbMemberHook<VmFreeBlock, offsetof(VmFreeBlock, node_addr)>,
    uintptr_t,
    VmFreeBlockAddrKeyOfValue
>;

using VmmSizeTree = IntrusiveRbTree<
    VmFreeBlock,
    detail::RbMemberHook<VmFreeBlock, offsetof(VmFreeBlock, node_size)>,
    VmFreeBlockSizeKey,
    VmFreeBlockSizeKeyOfValue,
    VmFreeBlockSizeKeyCompare
>;

class VmmState {
public:
    void init() noexcept;

    [[nodiscard]] void* alloc_pages(size_t count) noexcept;
    void free_pages(void* ptr, size_t count) noexcept;

    void reserve_range(uintptr_t start, size_t size) noexcept;

    [[nodiscard]] int map_page(uint32_t virt, uint32_t phys, uint32_t flags) noexcept;

    [[nodiscard]] size_t get_used_pages() const noexcept;

private:
    static constexpr uint32_t k_max_nodes = 4096u;

    SpinLock lock_;

    PmmState* pmm_ = nullptr;

    VmFreeBlock node_pool_[k_max_nodes]{};
    VmFreeBlock* free_nodes_head_ = nullptr;

    VmmAddrTree addr_tree_{};
    VmmSizeTree size_tree_{};

    atomic<size_t> used_pages_count_{0u};
};

[[nodiscard]] VmmState* vmm_state() noexcept;

} // namespace kernel

#endif

#ifdef __cplusplus
extern "C" {
#endif

void vmm_init(void);

void* vmm_alloc_pages(size_t pages);
void vmm_free_pages(void* virt, size_t pages);

void vmm_reserve_range(uint32_t virt, size_t size);

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

size_t vmm_get_used_pages(void);

#ifdef __cplusplus
}
#endif

#endif
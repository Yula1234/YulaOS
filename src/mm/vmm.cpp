// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/compiler.h>

#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>

#include <kernel/panic.h>

#include <lib/cpp/rbtree.h>
#include <lib/string.h>

#include <mm/pmm.h>

extern "C" {

#include <arch/i386/paging.h>

#include "vmm.h"

}

namespace {

struct VmFreeBlock {
    rb_node node_addr;
    rb_node node_size;

    uintptr_t start;
    size_t size;

    VmFreeBlock* next_free;
};

struct VmFreeBlockAddrKeyOfValue {
    const uintptr_t& operator()(const VmFreeBlock& block) const {
        return block.start;
    }
};

struct VmFreeBlockSizeKey {
    size_t size;
    uintptr_t start;
};

struct VmFreeBlockSizeKeyOfValue {
    VmFreeBlockSizeKey operator()(const VmFreeBlock& block) const {
        return VmFreeBlockSizeKey{block.size, block.start};
    }
};

struct VmFreeBlockSizeKeyCompare {
    bool operator()(const VmFreeBlockSizeKey& a, const VmFreeBlockSizeKey& b) const {
        if (a.size != b.size) {
            return a.size < b.size;
        }

        return a.start < b.start;
    }
};

class VmmState {
public:
    static constexpr uint32_t k_max_nodes = 4096u;

    void init() noexcept {
        pmm_ = kernel::pmm_state();

        init_node_pool();

        addr_tree_.clear();
        size_tree_.clear();

        used_pages_count_.store(0u, kernel::memory_order::relaxed);

        VmFreeBlock* initial = alloc_node();

        if (kernel::unlikely(!initial)) {
            panic("VMM: Out of metadata nodes during init!");
            return;
        }

        initial->start = static_cast<uintptr_t>(KERNEL_HEAP_START);
        initial->size = static_cast<size_t>(KERNEL_HEAP_SIZE);

        tree_insert(*initial);
    }

    [[nodiscard]] void* alloc_pages(size_t count) noexcept {
        if (kernel::unlikely(count == 0u)) {
            return nullptr;
        }

        if (kernel::unlikely(count > SIZE_MAX / PAGE_SIZE)) {
            return nullptr;
        }

        const size_t size_bytes = count * PAGE_SIZE;

        uintptr_t virt_base = 0u;

        {
            kernel::SpinLockSafeGuard guard(lock_);

            VmFreeBlock* block = find_best_fit(size_bytes);

            if (kernel::unlikely(!block)) {
                return nullptr;
            }

            virt_base = block->start;

            tree_erase(*block);

            if (block->size == size_bytes) {
                free_node(*block);
            } else {
                block->start += size_bytes;
                block->size -= size_bytes;

                tree_insert(*block);
            }

            used_pages_count_.fetch_add(count, kernel::memory_order::relaxed);
        }

        if (kernel::unlikely(!map_new_pages(virt_base, count))) {
            rollback_range(virt_base, count);
            return nullptr;
        }

        return reinterpret_cast<void*>(virt_base);
    }

    void free_pages(void* ptr, size_t count) noexcept {
        if (kernel::unlikely(!ptr || count == 0u)) {
            return;
        }

        const uintptr_t virt_base = reinterpret_cast<uintptr_t>(ptr);

        if (kernel::unlikely(count > SIZE_MAX / PAGE_SIZE)) {
            return;
        }

        const size_t size_bytes = count * PAGE_SIZE;

        kernel::SpinLockSafeGuard guard(lock_);

        for (size_t i = 0; i < count; i++) {
            const uintptr_t virt = virt_base + (i * PAGE_SIZE);

            const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(virt));
            if (kernel::likely(phys && pmm_)) {
                page_t* page = pmm_->phys_to_page(phys);
                if (kernel::unlikely(page && page->slab_cache)) {
                    panic("VMM: freeing slab page");
                }

                pmm_->free_pages(reinterpret_cast<void*>(static_cast<uintptr_t>(phys)), 0);
            }

            paging_map(kernel_page_directory, static_cast<uint32_t>(virt), 0, 0);
        }

        VmFreeBlock* block = alloc_node();
        if (kernel::unlikely(!block)) {
            panic("VMM: Out of metadata nodes during free!");
            return;
        }

        block->start = virt_base;
        block->size = size_bytes;

        tree_insert(*block);

        merge_adjacent(*block);

        used_pages_count_.fetch_sub(count, kernel::memory_order::relaxed);
    }

    [[nodiscard]] int map_page(uint32_t virt, uint32_t phys, uint32_t flags) noexcept {
        if (kernel::unlikely((virt & (PAGE_SIZE - 1u)) != 0u)) {
            return 0;
        }

        if (kernel::unlikely((phys & (PAGE_SIZE - 1u)) != 0u)) {
            return 0;
        }

        kernel::SpinLockSafeGuard guard(lock_);

        paging_map(kernel_page_directory, virt, phys, flags);

        return 1;
    }

    [[nodiscard]] size_t get_used_pages() const noexcept
    {
        return used_pages_count_.load(kernel::memory_order::relaxed);
    }

private:
    void init_node_pool() noexcept {
        for (size_t i = 0; i < k_max_nodes; i++) {
            init_block(node_pool_[i]);
        }

        for (size_t i = 0; i + 1u < k_max_nodes; i++) {
            node_pool_[i].next_free = &node_pool_[i + 1u];
        }

        node_pool_[k_max_nodes - 1u].next_free = nullptr;
        free_nodes_head_ = &node_pool_[0];
    }

    void init_block(VmFreeBlock& block) noexcept {
        block.node_addr.__parent_color = 0;
        block.node_addr.rb_left = nullptr;
        block.node_addr.rb_right = nullptr;

        block.node_size.__parent_color = 0;
        block.node_size.rb_left = nullptr;
        block.node_size.rb_right = nullptr;

        block.start = 0;
        block.size = 0;
        block.next_free = nullptr;
    }

    VmFreeBlock* alloc_node() noexcept {
        if (kernel::unlikely(!free_nodes_head_)) {
            return nullptr;
        }

        VmFreeBlock* node = free_nodes_head_;
        free_nodes_head_ = node->next_free;

        init_block(*node);

        return node;
    }

    void free_node(VmFreeBlock& node) noexcept {
        node.next_free = free_nodes_head_;
        free_nodes_head_ = &node;
    }

    void tree_insert(VmFreeBlock& block) noexcept {
        const bool inserted_addr = addr_tree_.insert_unique(block);
        const bool inserted_size = size_tree_.insert_unique(block);

        if (kernel::unlikely(!inserted_addr || !inserted_size)) {
            if (inserted_addr) {
                addr_tree_.erase(block);
            }

            if (inserted_size) {
                size_tree_.erase(block);
            }

            panic("VMM: rb-tree invariant violated (insert)");
        }
    }

    void tree_erase(VmFreeBlock& block) noexcept {
        addr_tree_.erase(block);
        size_tree_.erase(block);
    }

    void tree_reinsert_after_key_change(VmFreeBlock& block) noexcept {
        tree_erase(block);
        tree_insert(block);
    }

    void size_tree_reinsert_after_size_change(VmFreeBlock& block) noexcept {
        size_tree_.erase(block);

        const bool inserted = size_tree_.insert_unique(block);

        if (kernel::unlikely(!inserted)) {
            panic("VMM: rb-tree invariant violated (size reinsert)");
        }
    }

    VmFreeBlock* find_best_fit(size_t size) noexcept {
        const VmFreeBlockSizeKey key{size, 0u};

        auto it = size_tree_.lower_bound_key(key);

        if (it == size_tree_.end()) {
            return nullptr;
        }

        return &(*it);
    }

    bool map_new_pages(uintptr_t virt_base, size_t count) noexcept {
        if (kernel::unlikely(!pmm_)) {
            return false;
        }

        for (size_t i = 0; i < count; i++) {
            const uintptr_t virt = virt_base + (i * PAGE_SIZE);

            void* phys_ptr = pmm_->alloc_pages(0);
            const uint32_t phys = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(phys_ptr));

            if (kernel::unlikely(!phys)) {
                for (size_t j = 0; j < i; j++) {
                    const uintptr_t mapped_virt = virt_base + (j * PAGE_SIZE);
                    const uint32_t mapped_phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(mapped_virt));

                    if (mapped_phys) {
                        pmm_->free_pages(reinterpret_cast<void*>(static_cast<uintptr_t>(mapped_phys)), 0);
                    }

                    paging_map(kernel_page_directory, static_cast<uint32_t>(mapped_virt), 0, 0);
                }

                return false;
            }

            paging_map(kernel_page_directory, static_cast<uint32_t>(virt), phys, PTE_PRESENT | PTE_RW);
        }

        return true;
    }

    void rollback_range(uintptr_t virt_base, size_t count) noexcept {
        const size_t size_bytes = count * PAGE_SIZE;

        kernel::SpinLockSafeGuard guard(lock_);

        used_pages_count_.fetch_sub(count, kernel::memory_order::relaxed);

        VmFreeBlock* rollback = alloc_node();
        if (kernel::unlikely(!rollback)) {
            panic("VMM: Out of metadata nodes during rollback!");
            return;
        }

        rollback->start = virt_base;
        rollback->size = size_bytes;

        tree_insert(*rollback);

        merge_adjacent(*rollback);
    }

    void merge_adjacent(VmFreeBlock& block) noexcept {
        VmFreeBlock* curr = &block;

        bool merged = true;
        while (merged) {
            merged = false;

            auto it = addr_tree_.find_key(curr->start);
            if (kernel::unlikely(it == addr_tree_.end())) {
                panic("VMM: rb-tree invariant violated (merge/find)");
                return;
            }

            {
                auto next_it = it;
                ++next_it;

                if (next_it != addr_tree_.end()) {
                    VmFreeBlock* next = &(*next_it);

                    if (curr->start + curr->size == next->start) {
                        tree_erase(*next);

                        curr->size += next->size;
                        size_tree_reinsert_after_size_change(*curr);

                        free_node(*next);
                        merged = true;
                    }
                }
            }

            if (merged) {
                continue;
            }

            it = addr_tree_.find_key(curr->start);
            if (kernel::unlikely(it == addr_tree_.end())) {
                panic("VMM: rb-tree invariant violated (merge/refind)");
                return;
            }

            if (it != addr_tree_.begin()) {
                auto prev_it = it;
                --prev_it;

                VmFreeBlock* prev = &(*prev_it);

                if (prev->start + prev->size == curr->start) {
                    tree_erase(*curr);

                    prev->size += curr->size;
                    size_tree_reinsert_after_size_change(*prev);

                    free_node(*curr);

                    curr = prev;
                    merged = true;
                }
            }
        }
    }

    kernel::SpinLock lock_;

    kernel::PmmState* pmm_ = nullptr;

    VmFreeBlock node_pool_[k_max_nodes]{};
    VmFreeBlock* free_nodes_head_ = nullptr;

    kernel::IntrusiveRbTree<
        VmFreeBlock,
        kernel::detail::RbMemberHook<VmFreeBlock, offsetof(VmFreeBlock, node_addr)>,
        uintptr_t,
        VmFreeBlockAddrKeyOfValue
    > addr_tree_{};

    kernel::IntrusiveRbTree<
        VmFreeBlock,
        kernel::detail::RbMemberHook<VmFreeBlock, offsetof(VmFreeBlock, node_size)>,
        VmFreeBlockSizeKey,
        VmFreeBlockSizeKeyOfValue,
        VmFreeBlockSizeKeyCompare
    > size_tree_{};

    kernel::atomic<size_t> used_pages_count_{0u};
};

alignas(VmmState) static unsigned char g_vmm_storage[sizeof(VmmState)];
static VmmState* g_vmm = nullptr;

static inline VmmState& vmm_state_init_once() {
    if (!g_vmm) {
        g_vmm = new (g_vmm_storage) VmmState();
    }

    return *g_vmm;
}

static inline VmmState* vmm_state_if_inited() {
    return g_vmm;
}

}

extern "C" {

void vmm_init(void) {
    VmmState& vmm = vmm_state_init_once();

    vmm.init();
}

void* vmm_alloc_pages(size_t pages) {
    VmmState* vmm = vmm_state_if_inited();

    if (kernel::unlikely(!vmm)) {
        return nullptr;
    }

    return vmm->alloc_pages(pages);
}

void vmm_free_pages(void* virt, size_t pages) {
    VmmState* vmm = vmm_state_if_inited();

    if (kernel::unlikely(!vmm)) {
        return;
    }

    vmm->free_pages(virt, pages);
}

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    VmmState* vmm = vmm_state_if_inited();

    if (kernel::unlikely(!vmm)) {
        return 0;
    }

    return vmm->map_page(virt, phys, flags);
}

size_t vmm_get_used_pages(void) {
    VmmState* vmm = vmm_state_if_inited();

    if (kernel::unlikely(!vmm)) {
        return 0u;
    }

    return vmm->get_used_pages();
}

}

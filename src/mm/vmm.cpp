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
#include <mm/vmm.h>

extern "C" {

#include <arch/i386/paging.h>

}

namespace kernel {

const uintptr_t& VmFreeBlockAddrKeyOfValue::operator()(const VmFreeBlock& block) const noexcept {
    return block.start;
}

VmFreeBlockSizeKey VmFreeBlockSizeKeyOfValue::operator()(const VmFreeBlock& block) const noexcept {
    return VmFreeBlockSizeKey{block.size, block.start};
}

bool VmFreeBlockSizeKeyCompare::operator()(const VmFreeBlockSizeKey& a, const VmFreeBlockSizeKey& b) const noexcept {
    if (a.size != b.size) {
        return a.size < b.size;
    }

    return a.start < b.start;
}

namespace {

static void init_block(VmFreeBlock& block) noexcept {
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

static void init_node_pool(VmFreeBlock* pool, size_t count, VmFreeBlock*& head) noexcept {
    for (size_t i = 0; i < count; i++) {
        init_block(pool[i]);
    }

    for (size_t i = 0; i + 1u < count; i++) {
        pool[i].next_free = &pool[i + 1u];
    }

    pool[count - 1u].next_free = nullptr;
    head = &pool[0];
}

static VmFreeBlock* alloc_node(VmFreeBlock*& head) noexcept {
    if (kernel::unlikely(!head)) {
        return nullptr;
    }

    VmFreeBlock* node = head;
    head = node->next_free;

    init_block(*node);

    return node;
}

static void free_node(VmFreeBlock& node, VmFreeBlock*& head) noexcept {
    node.next_free = head;
    head = &node;
}

static void tree_insert(VmFreeBlock& block, VmmAddrTree& addr_tree, VmmSizeTree& size_tree) noexcept {
    const bool inserted_addr = addr_tree.insert_unique(block);
    const bool inserted_size = size_tree.insert_unique(block);

    if (kernel::unlikely(!inserted_addr || !inserted_size)) {
        if (inserted_addr) {
            addr_tree.erase(block);
        }

        if (inserted_size) {
            size_tree.erase(block);
        }

        panic("VMM: rb-tree invariant violated (insert)");
    }
}

static void tree_erase(VmFreeBlock& block, VmmAddrTree& addr_tree, VmmSizeTree& size_tree) noexcept {
    addr_tree.erase(block);
    size_tree.erase(block);
}

static void size_tree_reinsert_after_size_change(VmFreeBlock& block, VmmSizeTree& size_tree) noexcept {
    size_tree.erase(block);

    const bool inserted = size_tree.insert_unique(block);

    if (kernel::unlikely(!inserted)) {
        panic("VMM: rb-tree invariant violated (size reinsert)");
    }
}

static VmFreeBlock* find_best_fit(size_t size, VmmSizeTree& size_tree) noexcept {
    const VmFreeBlockSizeKey key{size, 0u};

    auto it = size_tree.lower_bound_key(key);

    if (it == size_tree.end()) {
        return nullptr;
    }

    return &(*it);
}

static bool map_new_pages(uintptr_t virt_base, size_t count, PmmState* pmm) noexcept {
    if (kernel::unlikely(!pmm)) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        const uintptr_t virt = virt_base + (i * PAGE_SIZE);

        void* phys_ptr = pmm->alloc_pages(0);
        const uint32_t phys = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(phys_ptr));

        if (kernel::unlikely(!phys)) {
            for (size_t j = 0; j < i; j++) {
                const uintptr_t mapped_virt = virt_base + (j * PAGE_SIZE);
                const uint32_t mapped_phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(mapped_virt));

                if (mapped_phys) {
                    pmm->free_pages(reinterpret_cast<void*>(static_cast<uintptr_t>(mapped_phys)), 0);
                }

                paging_map(kernel_page_directory, static_cast<uint32_t>(mapped_virt), 0, 0);
            }

            return false;
        }

        paging_map(kernel_page_directory, static_cast<uint32_t>(virt), phys, PTE_PRESENT | PTE_RW);
    }

    return true;
}

static void merge_adjacent(VmFreeBlock& block, VmmAddrTree& addr_tree, VmmSizeTree& size_tree, VmFreeBlock*& free_head) noexcept {
    VmFreeBlock* curr = &block;

    bool merged = true;
    while (merged) {
        merged = false;

        auto it = addr_tree.find_key(curr->start);
        if (kernel::unlikely(it == addr_tree.end())) {
            panic("VMM: rb-tree invariant violated (merge/find)");
            return;
        }

        {
            auto next_it = it;
            ++next_it;

            if (next_it != addr_tree.end()) {
                VmFreeBlock* next = &(*next_it);

                if (curr->start + curr->size == next->start) {
                    tree_erase(*next, addr_tree, size_tree);

                    curr->size += next->size;
                    size_tree_reinsert_after_size_change(*curr, size_tree);

                    free_node(*next, free_head);
                    merged = true;
                }
            }
        }

        if (merged) {
            continue;
        }

        it = addr_tree.find_key(curr->start);
        if (kernel::unlikely(it == addr_tree.end())) {
            panic("VMM: rb-tree invariant violated (merge/refind)");
            return;
        }

        if (it != addr_tree.begin()) {
            auto prev_it = it;
            --prev_it;

            VmFreeBlock* prev = &(*prev_it);

            if (prev->start + prev->size == curr->start) {
                tree_erase(*curr, addr_tree, size_tree);

                prev->size += curr->size;
                size_tree_reinsert_after_size_change(*prev, size_tree);

                free_node(*curr, free_head);

                curr = prev;
                merged = true;
            }
        }
    }
}

} // namespace

void VmmState::init() noexcept {
    pmm_ = kernel::pmm_state();

    init_node_pool(node_pool_, k_max_nodes, free_nodes_head_);

    addr_tree_.clear();
    size_tree_.clear();

    used_pages_count_.store(0u, kernel::memory_order::relaxed);

    VmFreeBlock* initial = alloc_node(free_nodes_head_);

    if (kernel::unlikely(!initial)) {
        panic("VMM: Out of metadata nodes during init!");
        return;
    }

    initial->start = static_cast<uintptr_t>(KERNEL_HEAP_START);
    initial->size = static_cast<size_t>(KERNEL_HEAP_SIZE);

    tree_insert(*initial, addr_tree_, size_tree_);
}

void* VmmState::alloc_pages(size_t count) noexcept {
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

        VmFreeBlock* block = find_best_fit(size_bytes, size_tree_);

        if (kernel::unlikely(!block)) {
            return nullptr;
        }

        virt_base = block->start;

        tree_erase(*block, addr_tree_, size_tree_);

        if (block->size == size_bytes) {
            free_node(*block, free_nodes_head_);
        } else {
            block->start += size_bytes;
            block->size -= size_bytes;

            tree_insert(*block, addr_tree_, size_tree_);
        }

        used_pages_count_.fetch_add(count, kernel::memory_order::relaxed);
    }

    if (kernel::unlikely(!map_new_pages(virt_base, count, pmm_))) {
        const size_t rollback_count = count;
        const size_t rollback_size = rollback_count * PAGE_SIZE;

        kernel::SpinLockSafeGuard guard(lock_);

        used_pages_count_.fetch_sub(rollback_count, kernel::memory_order::relaxed);

        VmFreeBlock* rollback = alloc_node(free_nodes_head_);
        if (kernel::unlikely(!rollback)) {
            panic("VMM: Out of metadata nodes during rollback!");
            return nullptr;
        }

        rollback->start = virt_base;
        rollback->size = rollback_size;

        tree_insert(*rollback, addr_tree_, size_tree_);

        merge_adjacent(*rollback, addr_tree_, size_tree_, free_nodes_head_);

        return nullptr;
    }

    return reinterpret_cast<void*>(virt_base);
}

void VmmState::free_pages(void* ptr, size_t count) noexcept {
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

    VmFreeBlock* block = alloc_node(free_nodes_head_);
    if (kernel::unlikely(!block)) {
        panic("VMM: Out of metadata nodes during free!");
        return;
    }

    block->start = virt_base;
    block->size = size_bytes;

    tree_insert(*block, addr_tree_, size_tree_);

    merge_adjacent(*block, addr_tree_, size_tree_, free_nodes_head_);

    used_pages_count_.fetch_sub(count, kernel::memory_order::relaxed);
}

int VmmState::map_page(uint32_t virt, uint32_t phys, uint32_t flags) noexcept {
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

size_t VmmState::get_used_pages() const noexcept {
    return used_pages_count_.load(kernel::memory_order::relaxed);
}

alignas(VmmState) static unsigned char g_vmm_storage[sizeof(VmmState)];
static VmmState* g_vmm = nullptr;

VmmState* vmm_state() noexcept {
    return g_vmm;
}

static inline VmmState& vmm_state_init_once() {
    if (!g_vmm) {
        g_vmm = new (g_vmm_storage) VmmState();
    }

    return *g_vmm;
}

} // namespace kernel

extern "C" {

void vmm_init(void) {
    kernel::VmmState& vmm = kernel::vmm_state_init_once();

    vmm.init();
}

void* vmm_alloc_pages(size_t pages) {
    kernel::VmmState* vmm = kernel::vmm_state();

    if (kernel::unlikely(!vmm)) {
        return nullptr;
    }

    return vmm->alloc_pages(pages);
}

void vmm_free_pages(void* virt, size_t pages) {
    kernel::VmmState* vmm = kernel::vmm_state();

    if (kernel::unlikely(!vmm)) {
        return;
    }

    vmm->free_pages(virt, pages);
}

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    kernel::VmmState* vmm = kernel::vmm_state();

    if (kernel::unlikely(!vmm)) {
        return 0;
    }

    return vmm->map_page(virt, phys, flags);
}

size_t vmm_get_used_pages(void) {
    kernel::VmmState* vmm = kernel::vmm_state();

    if (kernel::unlikely(!vmm)) {
        return 0u;
    }

    return vmm->get_used_pages();
}

}

/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */


#include <lib/cpp/lock_guard.h>
#include <lib/cpp/atomic.h>
#include <lib/compiler.h>
#include <lib/cpp/new.h>
#include <lib/string.h>

#include <hal/align.h>
#include <hal/cpu.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <kernel/panic.h>
#include <kernel/rcu.h>

extern "C" {

#include <arch/i386/paging.h>

void smp_tlb_shootdown_range(uint32_t start, uint32_t end);

}

___inline int vmm_cpu_index() noexcept {
    const int cpu = hal_cpu_index();
    if (cpu < 0 || cpu >= MAX_CPUS) {
        return 0;
    }

    return cpu;
}

struct VmmFreeBatch {
    rcu_head_t rcu;
    uintptr_t virt_base;
    size_t count;
    size_t phys_count;
    uint32_t phys_pages[0];
};

static void vmm_rcu_free_batch_cb(rcu_head_t* head) {
    VmmFreeBatch* batch = reinterpret_cast<VmmFreeBatch*>(
        reinterpret_cast<uintptr_t>(head) - offsetof(VmmFreeBatch, rcu)
    );

    kernel::PmmState* pmm = kernel::pmm_state();
    if (kernel::likely(pmm && batch->phys_count > 0)) {
        pmm->free_pages_order0_batch(
            reinterpret_cast<void* const*>(batch->phys_pages), 
            batch->phys_count
        );
    }

    kernel::VmmState* vmm = kernel::vmm_state();
    if (kernel::likely(vmm)) {
        vmm->unreserve_pages(reinterpret_cast<void*>(batch->virt_base), batch->count);
    }

    kfree(batch);
}

static void vmm_rcu_free_single_cb(rcu_head_t* head) {
    page_t* p = container_of(head, page_t, rcu);
    kernel::PmmState* pmm = kernel::pmm_state();
    
    uint32_t phys = pmm->page_to_phys(p);
    uintptr_t virt = reinterpret_cast<uintptr_t>(p->freelist);
    
    pmm->free_pages(reinterpret_cast<void*>(static_cast<uintptr_t>(phys)), 0u);

    kernel::VmmState* vmm = kernel::vmm_state();
    if (kernel::likely(vmm)) {
        vmm->rcu_free_single_complete(virt);
    }
}

namespace kernel {

namespace {

___inline size_t get_subtree_max_size(struct rb_node* node) {
    if (unlikely(!node)) {
        return 0;
    }

    return rb_entry(node, VmFreeBlock, node)->subtree_max_size;
}

___inline bool vmm_recalc_subtree(VmFreeBlock* block) {
    size_t old_max = block->subtree_max_size;
    size_t new_max = block->size;

    size_t l_max = get_subtree_max_size(block->node.rb_left);
    size_t r_max = get_subtree_max_size(block->node.rb_right);

    if (l_max > new_max) new_max = l_max;
    if (r_max > new_max) new_max = r_max;

    block->subtree_max_size = new_max;
    return old_max != new_max;
}

static void vmm_augment_propagate(struct rb_node* node, struct rb_node* stop) {
    while (node != stop) {
        VmFreeBlock* block = rb_entry(node, VmFreeBlock, node);
        vmm_recalc_subtree(block);
        node = rb_parent(node);
    }
}

___inline void vmm_augment_copy(struct rb_node* old_n, struct rb_node* new_n) {
    VmFreeBlock* old_b = rb_entry(old_n, VmFreeBlock, node);
    VmFreeBlock* new_b = rb_entry(new_n, VmFreeBlock, node);
    new_b->subtree_max_size = old_b->subtree_max_size;
}

static void vmm_augment_rotate(struct rb_node* old_n, struct rb_node* new_n) {
    VmFreeBlock* old_b = rb_entry(old_n, VmFreeBlock, node);
    VmFreeBlock* new_b = rb_entry(new_n, VmFreeBlock, node);

    vmm_recalc_subtree(old_b);
    vmm_recalc_subtree(new_b);
}

static const struct rb_augment_callbacks vmm_rb_callbacks = {
    .propagate = vmm_augment_propagate,
    .copy      = vmm_augment_copy,
    .rotate    = vmm_augment_rotate,
};

___inline void init_block(VmFreeBlock& block) noexcept {
    block.node.__parent_color = 0;
    block.node.rb_left = nullptr;
    block.node.rb_right = nullptr;

    block.start = 0;
    block.size = 0;
    block.subtree_max_size = 0;
    block.next_free = nullptr;
}

static void init_node_pool(VmFreeBlock* pool, size_t count, VmFreeBlock*& head) noexcept {
    /*
     * Node pool is a fixed-size metadata allocator.
     * We use it to avoid allocating heap memory while managing the heap.
     */
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

___inline void free_node(VmFreeBlock& node, VmFreeBlock*& head) noexcept {
    node.next_free = head;
    head = &node;
}

static void tree_insert(VmFreeBlock& block, struct rb_root* root) noexcept {
    struct rb_node** link = &root->rb_node;
    struct rb_node* parent = nullptr;

    while (*link) {
        parent = *link;
        VmFreeBlock* curr = rb_entry(parent, VmFreeBlock, node);

        if (block.start < curr->start) {
            link = &(*link)->rb_left;
        } else {
            link = &(*link)->rb_right;
        }
    }

    rb_link_node(&block.node, parent, link);
    block.subtree_max_size = block.size;

    if (parent) {
        vmm_augment_propagate(parent, nullptr);
    }

    rb_insert_color_augmented(&block.node, root, &vmm_rb_callbacks);
}

___inline void tree_erase(VmFreeBlock& block, struct rb_root* root) noexcept {
    rb_erase_augmented(&block.node, root, &vmm_rb_callbacks);
    block.node.__parent_color = 0;
    block.node.rb_left = nullptr;
    block.node.rb_right = nullptr;
}

static VmFreeBlock* find_fit(struct rb_root* root, size_t size) noexcept {
    if (!root->rb_node) return nullptr;

    if (get_subtree_max_size(root->rb_node) < size) {
        return nullptr;
    }

    struct rb_node* node = root->rb_node;

    while (node) {
        if (get_subtree_max_size(node->rb_left) >= size) {
            node = node->rb_left;
        } else {
            VmFreeBlock* curr = rb_entry(node, VmFreeBlock, node);
            if (curr->size >= size) {
                return curr;
            }
            node = node->rb_right;
        }
    }

    return nullptr;
}

static bool map_new_pages(uintptr_t virt_base, size_t count, kernel::PmmState* pmm) noexcept {
    if (kernel::unlikely(!pmm || count == 0u)) return false;

    size_t mapped_total = 0;

    while (mapped_total < count) {
        size_t remaining = count - mapped_total;
        uintptr_t cur_virt = virt_base + (mapped_total * PAGE_SIZE);

        if (remaining >= 1024 && (cur_virt & 0x3FFFFFu) == 0) {
            void* phys_4m = pmm->alloc_pages_zone(10, PMM_ZONE_NORMAL);
            if (phys_4m) {
                paging_map_4m(
                    kernel_page_directory,
                    static_cast<uint32_t>(cur_virt),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(phys_4m)),
                    PTE_PRESENT | PTE_RW
                );
                mapped_total += 1024;
                continue;
            }
        }

        size_t chunk = remaining > 128 ? 128 : remaining;

        void* phys_batch[128];
        uint32_t allocated = pmm->alloc_pages_order0_batch(PMM_ZONE_NORMAL, phys_batch, chunk);

        if (allocated > 0) {
            uint32_t phys_u32[128];

            for (size_t j = 0; j < allocated; j++) {
                phys_u32[j] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(phys_batch[j]));
            }

            paging_map_batch(
                kernel_page_directory,
                static_cast<uint32_t>(cur_virt),
                phys_u32,
                allocated,
                PTE_PRESENT | PTE_RW,
                PAGING_MAP_NO_TLB_FLUSH
            );
            mapped_total += allocated;
        }

        if (kernel::unlikely(allocated < chunk)) {
            struct RollbackCtx { kernel::PmmState* p; } ctx = { pmm };
            
            auto visitor = [](uint32_t virt, uint32_t pte, void* vctx) -> int {
                (void)virt;
                auto* c = static_cast<RollbackCtx*>(vctx);
                if ((pte & (1u << 7)) != 0u) {
                    uint32_t phys = pte & ~0x3FFFFFu;
                    if (phys) c->p->free_pages(reinterpret_cast<void*>(phys), 10);
                } else if ((pte & 1u) != 0u) {
                    uint32_t phys = pte & ~0xFFFu;
                    if (phys) c->p->free_pages(reinterpret_cast<void*>(phys), 0);
                }
                return 1;
            };

            if (mapped_total > 0) {
                paging_unmap_range_ex(
                    kernel_page_directory,
                    static_cast<uint32_t>(virt_base),
                    static_cast<uint32_t>(virt_base + mapped_total * PAGE_SIZE),
                    visitor, &ctx
                );
            }
            return false;
        }
    }
    return true;
}

static void merge_adjacent(VmFreeBlock& block, struct rb_root* root, VmFreeBlock*& free_head) noexcept {
    VmFreeBlock* curr = &block;

    struct rb_node* next_n = rb_next(&curr->node);
    if (next_n) {
        VmFreeBlock* next_b = rb_entry(next_n, VmFreeBlock, node);
        if (curr->start + curr->size == next_b->start) {
            tree_erase(*next_b, root);

            curr->size += next_b->size;
            vmm_augment_propagate(&curr->node, nullptr);

            free_node(*next_b, free_head);
        }
    }

    struct rb_node* prev_n = rb_prev(&curr->node);
    if (prev_n) {
        VmFreeBlock* prev_b = rb_entry(prev_n, VmFreeBlock, node);
        if (prev_b->start + prev_b->size == curr->start) {
            tree_erase(*curr, root);

            prev_b->size += curr->size;
            vmm_augment_propagate(&prev_b->node, nullptr);

            free_node(*curr, free_head);
            curr = prev_b;
        }
    }
}

} /* namespace */

void VmmState::init() noexcept {
    pmm_ = kernel::pmm_state();

    init_node_pool(node_pool_, k_max_nodes, free_nodes_head_);

    free_tree_ = RB_ROOT;

    used_pages_count_.store(0u, kernel::memory_order::relaxed);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        pcp_caches_[cpu].count = 0u;
    }

    VmFreeBlock* initial = alloc_node(free_nodes_head_);
    if (kernel::unlikely(!initial)) {
        panic("VMM: Out of metadata nodes during init!");
        return;
    }

    initial->start = static_cast<uintptr_t>(KERNEL_HEAP_START);
    initial->size = static_cast<size_t>(KERNEL_HEAP_SIZE);

    tree_insert(*initial, &free_tree_);
}

void* VmmState::alloc_pages(size_t count) noexcept {
    if (kernel::unlikely(count == 0u)) {
        return nullptr;
    }

    if (kernel::unlikely(count > SIZE_MAX / PAGE_SIZE)) {
        return nullptr;
    }

    static constexpr size_t k_pcp_refill_pages = 32u;

    if (count == 1u) {
        uintptr_t virt = 0u;

        {
            kernel::ScopedIrqDisable irq_guard;

            const int cpu = vmm_cpu_index();
            PerCpuVmmCache& cache = pcp_caches_[cpu];

            if (cache.count > 0u) {
                virt = cache.free_pages[--cache.count];
            }
        }

        if (virt != 0u) {
            if (kernel::unlikely(!map_new_pages(virt, 1u, pmm_))) {
                return nullptr;
            }

            used_pages_count_.fetch_add(1u, kernel::memory_order::relaxed);
            return reinterpret_cast<void*>(virt);
        }

        uintptr_t carve_base = 0u;
        size_t carve_pages = k_pcp_refill_pages;

        {
            kernel::SpinLockSafeGuard guard(lock_);

            VmFreeBlock* block = find_fit(&free_tree_, carve_pages * PAGE_SIZE);
            if (kernel::unlikely(!block)) {
                carve_pages = 1u;
                block = find_fit(&free_tree_, PAGE_SIZE);
            }

            if (kernel::unlikely(!block)) {
                return nullptr;
            }

            carve_base = block->start;

            tree_erase(*block, &free_tree_);

            const size_t carved_bytes = carve_pages * PAGE_SIZE;
            if (block->size == carved_bytes) {
                free_node(*block, free_nodes_head_);
            } else {
                block->start += carved_bytes;
                block->size -= carved_bytes;

                tree_insert(*block, &free_tree_);
            }
        }

        const uintptr_t out = carve_base;

        if (carve_pages > 1u) {
            kernel::ScopedIrqDisable irq_guard;

            const int cpu = vmm_cpu_index();
            PerCpuVmmCache& cache = pcp_caches_[cpu];

            const size_t cache_free = PerCpuVmmCache::k_capacity - cache.count;
            const size_t available = carve_pages - 1u;
            const size_t to_cache = (cache_free < available) ? cache_free : available;

            for (size_t i = 0; i < to_cache; i++) {
                cache.free_pages[cache.count++] = carve_base + ((i + 1u) * PAGE_SIZE);
            }

            const size_t remaining = available - to_cache;
            if (remaining != 0u) {
                const uintptr_t rem_start = carve_base + ((to_cache + 1u) * PAGE_SIZE);
                const size_t rem_size = remaining * PAGE_SIZE;

                kernel::SpinLockSafeGuard guard(lock_);

                VmFreeBlock* rem_block = alloc_node(free_nodes_head_);
                if (kernel::unlikely(!rem_block)) {
                    panic("VMM: Out of metadata nodes during alloc refill!");
                    return nullptr;
                }

                rem_block->start = rem_start;
                rem_block->size = rem_size;

                tree_insert(*rem_block, &free_tree_);
                merge_adjacent(*rem_block, &free_tree_, free_nodes_head_);
            }
        }

        if (kernel::unlikely(!map_new_pages(out, 1u, pmm_))) {
            const size_t rollback_size = PAGE_SIZE;

            kernel::SpinLockSafeGuard guard(lock_);

            VmFreeBlock* rollback = alloc_node(free_nodes_head_);
            if (kernel::unlikely(!rollback)) {
                panic("VMM: Out of metadata nodes during rollback!");
                return nullptr;
            }

            rollback->start = out;
            rollback->size = rollback_size;

            tree_insert(*rollback, &free_tree_);
            merge_adjacent(*rollback, &free_tree_, free_nodes_head_);

            return nullptr;
        }

        used_pages_count_.fetch_add(1u, kernel::memory_order::relaxed);
        return reinterpret_cast<void*>(out);
    }

    const size_t size_bytes = count * PAGE_SIZE;

    uintptr_t virt_base = 0u;

    {
        kernel::SpinLockSafeGuard guard(lock_);

        VmFreeBlock* block = find_fit(&free_tree_, size_bytes);
        if (kernel::unlikely(!block)) {
            return nullptr;
        }

        virt_base = block->start;

        tree_erase(*block, &free_tree_);

        if (block->size == size_bytes) {
            free_node(*block, free_nodes_head_);
        } else {
            block->start += size_bytes;
            block->size -= size_bytes;

            tree_insert(*block, &free_tree_);
        }

        used_pages_count_.fetch_add(count, kernel::memory_order::relaxed);
    }

    /*
     * Mapping is performed outside the allocator lock.
     * If it fails, we roll the virtual range back into the free trees.
     */
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

        tree_insert(*rollback, &free_tree_);

        merge_adjacent(*rollback, &free_tree_, free_nodes_head_);

        return nullptr;
    }

    return reinterpret_cast<void*>(virt_base);
}

void VmmState::flush_va_cache_overflow(uintptr_t* flush, size_t flush_count) noexcept {
    for (size_t i = 1; i < flush_count; i++) {
        uintptr_t key = flush[i];
        size_t j = i;

        while (j > 0u && flush[j - 1u] > key) {
            flush[j] = flush[j - 1u];
            j--;
        }

        flush[j] = key;
    }

    kernel::SpinLockSafeGuard guard(lock_);

    uintptr_t run_start = flush[0];
    uintptr_t run_end = run_start + PAGE_SIZE;

    for (size_t i = 1; i < flush_count; i++) {
        const uintptr_t a = flush[i];

        if (a == run_end) {
            run_end += PAGE_SIZE;
            continue;
        }

        VmFreeBlock* block = alloc_node(free_nodes_head_);
        
        if (kernel::unlikely(!block)) {
            panic("VMM: Out of metadata nodes during free!");
            return;
        }

        block->start = run_start;
        block->size = run_end - run_start;
        
        tree_insert(*block, &free_tree_);
        merge_adjacent(*block, &free_tree_, free_nodes_head_);

        run_start = a;
        run_end = a + PAGE_SIZE;
    }

    VmFreeBlock* block = alloc_node(free_nodes_head_);
    if (kernel::unlikely(!block)) {
        panic("VMM: Out of metadata nodes during free!");
        return;
    }

    block->start = run_start;
    block->size = run_end - run_start;
    
    tree_insert(*block, &free_tree_);
    merge_adjacent(*block, &free_tree_, free_nodes_head_);
}

void VmmState::rcu_free_single_complete(uintptr_t virt) noexcept {
    static constexpr size_t k_flush_pages = PerCpuVmmCache::k_capacity / 2u;

    uintptr_t flush[k_flush_pages] = {};
    size_t flush_count = 0u;

    {
        kernel::ScopedIrqDisable irq_guard;
        
        const int cpu = vmm_cpu_index();
        PerCpuVmmCache& cache = pcp_caches_[cpu];

        if (cache.count < PerCpuVmmCache::k_capacity) {
            cache.free_pages[cache.count++] = virt;
        } else {
            for (size_t i = 0; i < k_flush_pages; i++) {
                flush[i] = cache.free_pages[--cache.count];
            }

            flush_count = k_flush_pages;
            cache.free_pages[cache.count++] = virt;
        }
    }

    if (flush_count != 0u) {
        flush_va_cache_overflow(flush, flush_count);
    }
}

void VmmState::free_pages(void* ptr, size_t count) noexcept {
    if (kernel::unlikely(!ptr || count == 0u)) {
        return;
    }

    const uintptr_t virt_base = reinterpret_cast<uintptr_t>(ptr);
    if (kernel::unlikely(count > SIZE_MAX / PAGE_SIZE)) {
        return;
    }

    used_pages_count_.fetch_sub(count, kernel::memory_order::relaxed);

    if (count == 1u) {
        const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(virt_base));

        if (kernel::likely(phys && pmm_)) {
            page_t* page = pmm_->phys_to_page(phys);

            if (kernel::unlikely(page && page->slab_cache)) {
                panic("VMM: freeing slab page");
            }
        }

        paging_map_ex(kernel_page_directory, static_cast<uint32_t>(virt_base), 0u, 0u, PAGING_MAP_NO_TLB_FLUSH);

        if (phys != 0u && pmm_) {
            page_t* page = pmm_->phys_to_page(phys);
            if (page) {
                page->freelist = reinterpret_cast<void*>(virt_base);

                call_rcu(&page->rcu, vmm_rcu_free_single_cb);
                return;
            }
        }
        
        smp_tlb_shootdown_range(static_cast<uint32_t>(virt_base), static_cast<uint32_t>(virt_base + PAGE_SIZE));

        rcu_free_single_complete(virt_base);

        return;
    }

    size_t batch_size = sizeof(VmmFreeBatch) + count * sizeof(uint32_t);
    VmmFreeBatch* batch = (VmmFreeBatch*)kmalloc(batch_size);

    if (kernel::likely(batch != nullptr)) {
        batch->virt_base = virt_base;
        
        batch->count = count;
        batch->phys_count = 0;

        struct VmmUnmapCtx {
            VmmFreeBatch* b;

            kernel::PmmState* p;
        } ctx = { batch, pmm_ };

        auto visitor = [](uint32_t virt, uint32_t pte, void* vctx) -> int {
            auto* c = static_cast<VmmUnmapCtx*>(vctx);
            bool is_4m = (pte & (1u << 7)) != 0u;
            uint32_t phys = is_4m ? (pte & ~0x3FFFFFu) : (pte & ~0xFFFu);
            
            if (kernel::likely(phys && c->p)) {
                page_t* page = c->p->phys_to_page(phys);
                if (kernel::unlikely(page && page->slab_cache)) {
                    panic("VMM: freeing slab page");
                }
                if (is_4m) {
                    smp_tlb_shootdown_range(virt, virt + 0x400000u);
                    c->p->free_pages(reinterpret_cast<void*>(phys), 10);
                } else {
                    c->b->phys_pages[c->b->phys_count++] = phys;
                }
            }
            return 1;
        };

        paging_unmap_range_no_tlb(
            kernel_page_directory, static_cast<uint32_t>(virt_base),
            static_cast<uint32_t>(virt_base + count * PAGE_SIZE),
            visitor, &ctx
        );

        call_rcu(&batch->rcu, vmm_rcu_free_batch_cb);
        return;
    }

    static constexpr size_t k_batch = 128u;
    
    for (size_t offset = 0; offset < count; offset += k_batch) {
        size_t chunk = count - offset;
        if (chunk > k_batch) {
            chunk = k_batch;
        }

        struct VmmSyncCtx {
            uint32_t phys_batch[k_batch];
            size_t phys_count;

            kernel::PmmState* p;
        } ctx = { {}, 0, pmm_ };

        auto visitor = [](uint32_t virt, uint32_t pte, void* vctx) -> int {
            (void)virt;
            
            auto* c = static_cast<VmmSyncCtx*>(vctx);
            bool is_4m = (pte & (1u << 7)) != 0u;
            uint32_t phys = is_4m ? (pte & ~0x3FFFFFu) : (pte & ~0xFFFu);
            
            if (kernel::likely(phys && c->p)) {
                page_t* page = c->p->phys_to_page(phys);
                if (kernel::unlikely(page && page->slab_cache)) {
                    panic("VMM: freeing slab page");
                }
                if (is_4m) {
                    c->p->free_pages(reinterpret_cast<void*>(phys), 10);
                } else {
                    c->phys_batch[c->phys_count++] = phys;
                }
            }
            return 1;
        };

        const uintptr_t chunk_virt = virt_base + (offset * PAGE_SIZE);

        paging_unmap_range_ex(
            kernel_page_directory, static_cast<uint32_t>(chunk_virt),
            static_cast<uint32_t>(chunk_virt + chunk * PAGE_SIZE),
            visitor, &ctx
        );

        for (size_t k = 0u; k < ctx.phys_count; k++) {
            pmm_->free_pages(reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.phys_batch[k])), 0u);
        }
    }

    unreserve_pages(reinterpret_cast<void*>(virt_base), count);
}

int VmmState::map_page(uint32_t virt, uint32_t phys, uint32_t flags) noexcept {
    /*
     * Low-level helper for paging_map().
     * Caller is responsible for choosing flags (e.g. PTE_PRESENT/PTE_RW).
     */
    if (kernel::unlikely((virt & (PAGE_SIZE - 1u)) != 0u)) {
        return 0;
    }

    if (kernel::unlikely((phys & (PAGE_SIZE - 1u)) != 0u)) {
        return 0;
    }

    paging_map(kernel_page_directory, virt, phys, flags);

    return 1;
}

void* VmmState::reserve_pages(size_t count) noexcept {
    if (kernel::unlikely(count == 0u || count > SIZE_MAX / PAGE_SIZE)) {
        return nullptr;
    }

    const size_t size_bytes = count * PAGE_SIZE;
    uintptr_t virt_base = 0u;

    kernel::SpinLockSafeGuard guard(lock_);

    VmFreeBlock* block = find_fit(&free_tree_, size_bytes);
    if (kernel::unlikely(!block)) {
        return nullptr;
    }

    virt_base = block->start;
    tree_erase(*block, &free_tree_);

    if (block->size == size_bytes) {
        free_node(*block, free_nodes_head_);
    } else {
        block->start += size_bytes;
        block->size -= size_bytes;

        tree_insert(*block, &free_tree_);
    }
    
    return reinterpret_cast<void*>(virt_base);
}

void VmmState::unreserve_pages(void* ptr, size_t count) noexcept {
    if (kernel::unlikely(!ptr || count == 0u || count > SIZE_MAX / PAGE_SIZE)) {
        return;
    }

    const uintptr_t virt_base = reinterpret_cast<uintptr_t>(ptr);
    const size_t size_bytes = count * PAGE_SIZE;
    
    kernel::SpinLockSafeGuard guard(lock_);

    VmFreeBlock* block = alloc_node(free_nodes_head_);
    if (kernel::unlikely(!block)) {
        panic("VMM: Out of metadata nodes during unreserve.");
        return;
    }

    block->start = virt_base;
    block->size = size_bytes;

    tree_insert(*block, &free_tree_);
    merge_adjacent(*block, &free_tree_, free_nodes_head_);
}

size_t VmmState::get_used_pages() const noexcept {
    return used_pages_count_.load(kernel::memory_order::relaxed);
}

alignas(VmmState) static unsigned char g_vmm_storage[sizeof(VmmState)];
static VmmState* g_vmm = nullptr;

VmmState* vmm_state() noexcept {
    return g_vmm;
}

static inline VmmState& vmm_state_init_once() noexcept {
    /* Construct on first use to avoid init-order constraints during boot. */
    if (!g_vmm) {
        g_vmm = new (g_vmm_storage) VmmState();
    }

    return *g_vmm;
}

} /* namespace kernel */

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

void* vmm_reserve_pages(size_t pages) {
    kernel::VmmState* vmm = kernel::vmm_state();
    return kernel::likely(vmm) ? vmm->reserve_pages(pages) : nullptr;
}

void vmm_unreserve_pages(void* virt, size_t pages) {
    kernel::VmmState* vmm = kernel::vmm_state();
    if (kernel::likely(vmm)) vmm->unreserve_pages(virt, pages);
}

size_t vmm_get_used_pages(void) {
    kernel::VmmState* vmm = kernel::vmm_state();

    if (kernel::unlikely(!vmm)) {
        return 0u;
    }

    return vmm->get_used_pages();
}

}

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <mm/vma.h>

#include <arch/i386/paging.h>

#include <fs/vfs.h>

#include <kernel/proc.h>

#include <mm/pmm.h>

#include <mm/heap.h>

#include <lib/cpp/new.h>

#include <lib/cpp/lock_guard.h>

extern "C" void smp_tlb_shootdown_range(uint32_t start, uint32_t end);

namespace {

constexpr uint32_t page_size = 4096u;
constexpr uint32_t page_mask = 4095u;

constexpr uint32_t user_addr_min = 0x40000000u;
constexpr uint32_t user_addr_max = 0xC0000000u;

constexpr uint32_t user_stack_addr_min = 0x60000000u;
constexpr uint32_t user_stack_addr_max = 0x80000000u;

static kmem_cache_t* g_vma_region_cache = nullptr;

static inline uint32_t align_down_4k(uint32_t v) noexcept {
    return v & ~page_mask;
}

static inline uint32_t align_up_4k(uint32_t v) noexcept {
    return (v + page_mask) & ~page_mask;
}

static inline bool ranges_overlap(uint32_t a_start, uint32_t a_end, uint32_t b_start, uint32_t b_end) noexcept {
    return (a_start < b_end) && (b_start < a_end);
}

static inline vma_region_t* rb_to_region(rb_node* node) noexcept {
    return node ? rb_entry(node, vma_region_t, rb_node) : nullptr;
}

static inline const vma_region_t* rb_to_region(const rb_node* node) noexcept {
    return node ? rb_entry(node, const vma_region_t, rb_node) : nullptr;
}

static inline uint32_t region_span(const vma_region_t* region) noexcept {
    return region ? (region->vaddr_end - region->vaddr_start) : 0u;
}

static void tree_erase(proc_mem_t* mem, vma_region_t* region) noexcept;

static void free_region(vma_region_t* region) noexcept;

static void apply_child_metrics(
    const vma_region_t* child,
    uint32_t& min_start, uint32_t& max_end, uint32_t& max_gap,
    uint32_t parent_edge, bool is_left) noexcept;

static bool vma_region_recalc(vma_region_t* region) noexcept {
    if (!region) {
        return false;
    }

    const uint32_t old_min_start = region->subtree_min_start;
    const uint32_t old_max_end = region->subtree_max_end;
    const uint32_t old_max_gap = region->subtree_max_gap;

    uint32_t min_start = region->vaddr_start;
    uint32_t max_end = region->vaddr_end;
    uint32_t max_gap = 0u;

    apply_child_metrics(
        rb_to_region(region->rb_node.rb_left),
        min_start, max_end, max_gap,
        region->vaddr_start, true
    );

    apply_child_metrics(
        rb_to_region(region->rb_node.rb_right),
        min_start, max_end, max_gap,
        region->vaddr_end, false
    );

    region->subtree_min_start = min_start;
    region->subtree_max_end = max_end;
    region->subtree_max_gap = max_gap;

    return old_min_start != min_start
        || old_max_end != max_end
        || old_max_gap != max_gap;
}

static void vma_region_propagate(rb_node* node, rb_node* stop) {
    for (rb_node* cur = node; cur && cur != stop; cur = rb_parent(cur)) {
        vma_region_t* region = rb_to_region(cur);
        if (!region) {
            continue;
        }

        if (!vma_region_recalc(region)) {
            break;
        }
    }
}

static void vma_region_copy(rb_node* old, rb_node* new_node) {
    vma_region_t* src = rb_to_region(old);
    vma_region_t* dst = rb_to_region(new_node);
    if (!src || !dst) {
        return;
    }

    dst->subtree_min_start = src->subtree_min_start;
    dst->subtree_max_end = src->subtree_max_end;
    dst->subtree_max_gap = src->subtree_max_gap;
}

static void vma_region_rotate(rb_node* old, rb_node* new_node) {
    (void)vma_region_recalc(rb_to_region(old));
    (void)vma_region_recalc(rb_to_region(new_node));
}

static const rb_augment_callbacks vma_rb_callbacks = {
    .propagate = vma_region_propagate,
    .copy = vma_region_copy,
    .rotate = vma_region_rotate,
};

static vma_region_t* alloc_region() noexcept;

static vma_region_t* create_initialized_region(
    uint32_t start, uint32_t end, uint32_t length, uint32_t flags,
    vfs_node_t* file, uint32_t file_offset, uint32_t file_size) noexcept
{
    vma_region_t* r = alloc_region();
    
    if (!r) {
        return nullptr;
    }

    r->vaddr_start = start;
    r->vaddr_end = end;
    r->length = length;
    r->map_flags = flags;
    r->file = file;
    r->file_offset = file_offset;
    r->file_size = file_size;

    r->subtree_min_start = start;
    r->subtree_max_end = end;
    r->subtree_max_gap = 0u;

    r->rb_node.__parent_color = 0;
    r->rb_node.rb_left = nullptr;
    r->rb_node.rb_right = nullptr;
    r->list_node.next = nullptr;
    r->list_node.prev = nullptr;

    return r;
}

static void adjust_file_bounds(
    vma_region_t* region, uint32_t cut_len, uint32_t new_len, bool cut_from_left) noexcept
{
    if (!region->file) {
        region->file_offset = 0u;
        region->file_size = 0u;
        return;
    }

    if (cut_from_left) {
        region->file_offset += cut_len;
        
        if (region->file_size > cut_len) {
            region->file_size -= cut_len;
        } else {
            region->file_size = 0u;
        }
    }

    if (region->file_size > new_len) {
        region->file_size = new_len;
    }
}

static void apply_child_metrics(
    const vma_region_t* child,
    uint32_t& min_start, uint32_t& max_end, uint32_t& max_gap,
    uint32_t parent_edge, bool is_left) noexcept
{
    if (!child) {
        return;
    }

    if (child->subtree_min_start < min_start) {
        min_start = child->subtree_min_start;
    }

    if (child->subtree_max_end > max_end) {
        max_end = child->subtree_max_end;
    }

    if (child->subtree_max_gap > max_gap) {
        max_gap = child->subtree_max_gap;
    }

    if (is_left && child->subtree_max_end < parent_edge) {
        uint32_t gap = parent_edge - child->subtree_max_end;
        
        if (gap > max_gap) {
            max_gap = gap;
        }
    } else if (!is_left && parent_edge < child->subtree_min_start) {
        uint32_t gap = child->subtree_min_start - parent_edge;
        
        if (gap > max_gap) {
            max_gap = gap;
        }
    }
}

static bool regions_are_mergeable(const vma_region_t* left, const vma_region_t* right) noexcept {
    if (!left || !right) {
        return false;
    }

    if (left->vaddr_end != right->vaddr_start) {
        return false;
    }

    if (left->map_flags != right->map_flags) {
        return false;
    }

    if (left->file != right->file) {
        return false;
    }

    if (left->file) {
        uint32_t left_span = region_span(left);

        if (left->file_offset + left_span != right->file_offset) {
            return false;
        }
    }

    return true;
}

static void merge_regions_into_left(proc_mem_t* mem, vma_region_t* left, vma_region_t* right) noexcept {
    if (!mem || !left || !right) {
        return;
    }

    uint32_t left_span = region_span(left);
    uint32_t right_span = region_span(right);
    uint32_t merged_span = left_span + right_span;

    left->vaddr_end = right->vaddr_end;
    left->length = merged_span;

    uint32_t merged_file_size = 0u;
    if (left->file) {
        if (left->file_size > 0xFFFFFFFFu - right->file_size) {
            merged_file_size = 0xFFFFFFFFu;
        } else {
            merged_file_size = left->file_size + right->file_size;
        }
        if (merged_file_size > merged_span) {
            merged_file_size = merged_span;
        }
    }
    left->file_size = merged_file_size;

    vma_region_propagate(&left->rb_node, nullptr);

    dlist_del(&right->list_node);
    tree_erase(mem, right);
    free_region(right);
}

static vma_region_t* tree_find_leq(proc_mem_t* mem, uint32_t vaddr) noexcept {
    if (!mem) {
        return nullptr;
    }

    rb_node* node = mem->mmap_tree.rb_node;
    vma_region_t* best = nullptr;

    while (node) {
        vma_region_t* cur = rb_to_region(node);
        if (!cur) {
            break;
        }

        if (vaddr < cur->vaddr_start) {
            node = node->rb_left;
            continue;
        }

        best = cur;
        node = node->rb_right;
    }

    return best;
}

static int tree_insert(proc_mem_t* mem, vma_region_t* region) noexcept {
    if (!mem || !region) {
        return 0;
    }

    rb_node** link = &mem->mmap_tree.rb_node;
    rb_node* parent = nullptr;

    while (*link) {
        parent = *link;
        vma_region_t* cur = rb_to_region(parent);

        if (region->vaddr_start < cur->vaddr_start) {
            link = &((*link)->rb_left);
            continue;
        }

        if (region->vaddr_start > cur->vaddr_start) {
            link = &((*link)->rb_right);
            continue;
        }

        return 0;
    }

    rb_link_node(&region->rb_node, parent, link);

    region->subtree_min_start = region->vaddr_start;
    region->subtree_max_end = region->vaddr_end;
    region->subtree_max_gap = 0u;

    rb_insert_color_augmented(&region->rb_node, &mem->mmap_tree, &vma_rb_callbacks);
    return 1;
}

static void tree_erase(proc_mem_t* mem, vma_region_t* region) noexcept {
    if (!mem || !region) {
        return;
    }

    vma_region_t* cached = __atomic_load_n(&mem->mmap_cache, __ATOMIC_RELAXED);
    if (cached == region) {
        __atomic_store_n(&mem->mmap_cache, static_cast<vma_region_t*>(nullptr), __ATOMIC_RELAXED);
    }

    rb_erase_augmented(&region->rb_node, &mem->mmap_tree, &vma_rb_callbacks);
    region->rb_node.__parent_color = 0;
    region->rb_node.rb_left = nullptr;
    region->rb_node.rb_right = nullptr;
}

static vma_region_t* vma_find_unlocked(proc_mem_t* mem, uint32_t vaddr) noexcept {
    if (!mem) {
        return nullptr;
    }

    vma_region_t* cached = __atomic_load_n(&mem->mmap_cache, __ATOMIC_RELAXED);
    if (cached && vaddr >= cached->vaddr_start && vaddr < cached->vaddr_end) {
        return cached;
    }

    vma_region_t* cand = tree_find_leq(mem, vaddr);
    if (!cand) {
        return nullptr;
    }

    if (vaddr >= cand->vaddr_start && vaddr < cand->vaddr_end) {
        __atomic_store_n(&mem->mmap_cache, cand, __ATOMIC_RELAXED);
        return cand;
    }

    return nullptr;
}

static vma_region_t* vma_find_overlap_unlocked(proc_mem_t* mem, uint32_t start, uint32_t end_excl) noexcept {
    if (!mem || end_excl <= start) {
        return nullptr;
    }

    vma_region_t* cand = tree_find_leq(mem, start);
    if (cand && ranges_overlap(start, end_excl, cand->vaddr_start, cand->vaddr_end)) {
        return cand;
    }

    if (cand) {
        cand = rb_to_region(rb_next(&cand->rb_node));
    } else {
        cand = rb_to_region(rb_first(&mem->mmap_tree));
    }

    if (cand && ranges_overlap(start, end_excl, cand->vaddr_start, cand->vaddr_end)) {
        return cand;
    }

    return nullptr;
}

static void unmap_range(uint32_t* page_dir, uint32_t start, uint32_t end) noexcept {
    const uint32_t aligned_start = align_down_4k(start);
    const uint32_t aligned_end = align_up_4k(end);

    struct UnmapCtx {
        uint32_t freed_pages = 0u;
    } ctx{};

    auto visitor =[](uint32_t /*virt*/, uint32_t pte, void* vctx) -> int {
        if ((pte & 4u) == 0u) {
            return 0;
        }

        bool is_4m = (pte & (1u << 7)) != 0u;
        uint32_t phys = is_4m ? (pte & ~0x3FFFFFu) : (pte & ~0xFFFu);
        
        if (phys != 0u && (pte & 0x200u) == 0u) {
            if (is_4m) {
                pmm_free_pages((void*)phys, 10);
            } else {
                pmm_free_block_deferred((void*)phys);
            }
        }

        auto* ctxp = static_cast<UnmapCtx*>(vctx);
        if (ctxp) {
            ctxp->freed_pages += is_4m ? 1024 : 1;
        }

        return 1;
    };

    paging_unmap_range_ex(page_dir, start, end, visitor, &ctx);

    (void)aligned_start;
    (void)aligned_end;
}

static void release_region_file(vma_region_t* region) noexcept {
    if (region && region->file) {
        vfs_node_release(region->file);
        region->file = nullptr;
    }
}

static vma_region_t* alloc_region() noexcept {
    if (!g_vma_region_cache) {
        return nullptr;
    }

    void* obj = kmem_cache_alloc(g_vma_region_cache);
    if (!obj) {
        return nullptr;
    }

    vma_region_t* region = new (obj) vma_region_t{};
    return region;
}

static void free_region(vma_region_t* region) noexcept {
    if (region) {
        release_region_file(region);
        region->~vma_region_t();
        kmem_cache_free(g_vma_region_cache, region);
    }
}

struct UnmapSpanCollector {
    struct span_t {
        uint32_t start;
        uint32_t end;
    };

    span_t stack_spans[8];
    span_t* spans = stack_spans;
    uint32_t cap = 8u;
    uint32_t len = 0u;
    bool use_heap = false;

    bool init(uint32_t needed) noexcept {
        if (needed > 8u) {
            spans = new (kernel::nothrow) span_t[needed]{};
            if (!spans) {
                return false;
            }
            cap = needed;
            use_heap = true;
        }
        return true;
    }

    void push(uint32_t start, uint32_t end) noexcept {
        if (start >= end) {
            return;
        }

        if (len > 0u && start <= spans[len - 1u].end) {
            if (end > spans[len - 1u].end) {
                spans[len - 1u].end = end;
            }
            return;
        }

        if (len < cap) {
            spans[len++] = span_t{start, end};
        }
    }

    void cleanup() noexcept {
        if (use_heap) {
            delete[] spans;
        }
    }
};

}

extern "C" void vma_init(proc_mem_t* mem) {
    if (!mem) {
        return;
    }

    if (!g_vma_region_cache) {
        g_vma_region_cache = kmem_cache_create("vma", sizeof(vma_region_t), 0u, 0u);
    }

    rwspinlock_init(&mem->mmap_lock);

    mem->mmap_tree = RB_ROOT;
    dlist_init(&mem->mmap_regions);

    __atomic_store_n(&mem->mmap_cache, static_cast<vma_region_t*>(nullptr), __ATOMIC_RELAXED);

    if (mem->mmap_top == 0u) {
        mem->mmap_top = user_addr_min;
    }

    if (mem->free_area_cache == 0u) {
        mem->free_area_cache = 0xB0000000u;
    }
}

extern "C" void vma_destroy(proc_mem_t* mem) {
    if (!mem) {
        return;
    }

    __atomic_store_n(&mem->mmap_cache, static_cast<vma_region_t*>(nullptr), __ATOMIC_RELAXED);

    while (true) {
        vma_region_t* victim = nullptr;

        {
            kernel::RwSpinLockNativeWriteGuard guard(mem->mmap_lock);

            if (dlist_empty(&mem->mmap_regions)) {
                mem->mmap_tree = RB_ROOT;
                break;
            }

            dlist_head_t* first = mem->mmap_regions.next;
            victim = container_of(first, vma_region_t, list_node);

            dlist_del(&victim->list_node);
            tree_erase(mem, victim);
        }

        free_region(victim);
    }
}

extern "C" vma_region_t* vma_create(
    proc_mem_t* mem,
    uint32_t vaddr,
    uint32_t size,
    vfs_node_t* file,
    uint32_t file_offset,
    uint32_t file_size,
    uint32_t flags
) {
    if (!mem) {
        return nullptr;
    }

    if (size == 0u) {
        return nullptr;
    }

    uint32_t aligned_vaddr = align_down_4k(vaddr);
    uint32_t diff = vaddr - aligned_vaddr;

    uint32_t aligned_size = align_up_4k(size + diff);

    uint32_t aligned_file_size = file_size;
    if (aligned_file_size > 0xFFFFFFFFu - diff) {
        aligned_file_size = 0xFFFFFFFFu;
    } else {
        aligned_file_size += diff;
    }

    if (aligned_file_size > aligned_size) {
        aligned_file_size = aligned_size;
    }

    if (file) {
        vfs_node_retain(file);
    }

    vma_region_t* region = create_initialized_region(
        aligned_vaddr, aligned_vaddr + aligned_size,
        size, flags, file, file_offset - diff, aligned_file_size
    );

    if (!region) {
        if (file) {
            vfs_node_release(file);
        }
        return nullptr;
    }

    bool has_overlap = false;

    {
        kernel::RwSpinLockNativeWriteGuard guard(mem->mmap_lock);

        has_overlap = vma_find_overlap_unlocked(mem, region->vaddr_start, region->vaddr_end) != nullptr;

        if (!has_overlap) {
            if (!tree_insert(mem, region)) {
                has_overlap = true;
            } else {
                dlist_add_tail(&region->list_node, &mem->mmap_regions);

                vma_region_t* merged = region;

                if (vma_region_t* prev = rb_to_region(rb_prev(&merged->rb_node))) {
                    if (regions_are_mergeable(prev, merged)) {
                        merge_regions_into_left(mem, prev, merged);
                        merged = prev;
                    }
                }

                while (true) {
                    vma_region_t* next = rb_to_region(rb_next(&merged->rb_node));
                    if (!next) {
                        break;
                    }

                    if (!regions_are_mergeable(merged, next)) {
                        break;
                    }

                    merge_regions_into_left(mem, merged, next);
                }

                region = merged;
            }

            if (mem->mmap_top < region->vaddr_end) {
                mem->mmap_top = region->vaddr_end;
            }
        }
    }

    if (has_overlap) {
        free_region(region);
        return nullptr;
    }

    __atomic_store_n(&mem->mmap_cache, region, __ATOMIC_RELAXED);

    return region;
}

extern "C" vma_region_t* vma_find(proc_mem_t* mem, uint32_t vaddr) {
    if (!mem) {
        return nullptr;
    }

    kernel::RwSpinLockNativeReadGuard guard(mem->mmap_lock);
    return vma_find_unlocked(mem, vaddr);
}

extern "C" vma_region_t* vma_find_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem) {
        return nullptr;
    }

    kernel::RwSpinLockNativeReadGuard guard(mem->mmap_lock);
    return vma_find_overlap_unlocked(mem, start, end_excl);
}

extern "C" int vma_has_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    return vma_find_overlap(mem, start, end_excl) != nullptr ? 1 : 0;
}

extern "C" int vma_remove(proc_mem_t* mem, uint32_t vaddr, uint32_t len) {
    if (!mem || !mem->page_dir || (vaddr & page_mask) || len == 0u || vaddr + len < vaddr) {
        return -1;
    }

    __atomic_store_n(&mem->mmap_cache, static_cast<vma_region_t*>(nullptr), __ATOMIC_RELAXED);

    uint32_t vaddr_end = vaddr + align_up_4k(len);
    UnmapSpanCollector collector;

    {
        kernel::RwSpinLockNativeWriteGuard guard(mem->mmap_lock);

        uint32_t need_spans = 0u;
        uint32_t scan = vaddr;

        while (scan < vaddr_end) {
            vma_region_t* r = vma_find_unlocked(mem, scan);
            if (!r || r->vaddr_end <= scan) {
                return -1;
            }
            need_spans++;
            scan = r->vaddr_end;
        }

        if (need_spans == 0u) {
            return 0;
        }

        if (!collector.init(need_spans)) {
            return -1;
        }

        scan = vaddr;
        while (scan < vaddr_end) {
            vma_region_t* curr = vma_find_unlocked(mem, scan);
            if (!curr) {
                collector.cleanup();
                return -1;
            }

            const uint32_t m_start = curr->vaddr_start;
            const uint32_t m_end = curr->vaddr_end;
            const uint32_t o_start = (vaddr > m_start) ? vaddr : m_start;
            const uint32_t o_end = (vaddr_end < m_end) ? vaddr_end : m_end;

            if (o_start >= o_end) {
                scan = m_end;
                continue;
            }

            collector.push(o_start, o_end);

            if (o_start > m_start && o_end < m_end) {
                uint32_t right_len = m_end - o_end;
                uint32_t right_off = curr->file_offset + (o_end - m_start);

                vma_region_t* new_right = create_initialized_region(
                    o_end, m_end, right_len, curr->map_flags,
                    curr->file, curr->file ? right_off : 0u, 0u
                );

                if (!new_right) {
                    collector.cleanup();
                    return -1;
                }

                if (curr->file) {
                    vfs_node_retain(curr->file);
                }

                adjust_file_bounds(new_right, o_end - m_start, right_len, true);

                tree_erase(mem, curr);
                curr->vaddr_end = o_start;
                curr->length = o_start - m_start;
                adjust_file_bounds(curr, 0u, curr->length, false);

                if (!tree_insert(mem, curr) || !tree_insert(mem, new_right)) {
                    free_region(new_right);
                    collector.cleanup();
                    return -1;
                }

                dlist_add_tail(&new_right->list_node, &mem->mmap_regions);
                scan = o_end;
                continue;
            }

            if (o_start == m_start && o_end == m_end) {
                dlist_del(&curr->list_node);
                tree_erase(mem, curr);
                free_region(curr);
                scan = o_end;
                continue;
            }

            if (o_start == m_start && o_end < m_end) {
                tree_erase(mem, curr);
                curr->vaddr_start = o_end;
                curr->length = m_end - o_end;
                adjust_file_bounds(curr, o_end - m_start, curr->length, true);

                if (!tree_insert(mem, curr)) {
                    collector.cleanup();
                    return -1;
                }
                scan = o_end;
                continue;
            }

            if (o_start > m_start && o_end == m_end) {
                tree_erase(mem, curr);
                curr->vaddr_end = o_start;
                curr->length = o_start - m_start;
                adjust_file_bounds(curr, 0u, curr->length, false);

                if (!tree_insert(mem, curr)) {
                    collector.cleanup();
                    return -1;
                }
                scan = o_end;
                continue;
            }

            scan = o_end;
        }
    }

    for (uint32_t i = 0u; i < collector.len; i++) {
        unmap_range(mem->page_dir, collector.spans[i].start, collector.spans[i].end);
    }

    collector.cleanup();

    if (mem->free_area_cache > vaddr) {
        mem->free_area_cache = vaddr;
    }

    return 0;
}

extern "C" int vma_validate_range(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem || !mem->page_dir) {
        return 0;
    }

    kernel::RwSpinLockNativeReadGuard guard(mem->mmap_lock);

    if (end_excl <= start) {
        return 1;
    }

    if (start < user_addr_min || end_excl > user_addr_max) {
        return 0;
    }

    uint32_t cur = start;

    while (cur < end_excl) {
        vma_region_t* region = vma_find_unlocked(mem, cur);

        if (!region) {
            return 0;
        }

        if (region->vaddr_start >= region->vaddr_end) {
            return 0;
        }

        uint32_t lim = region->vaddr_end;
        cur = (end_excl < lim) ? end_excl : lim;
    }

    return 1;
}

extern "C" uint32_t vma_alloc_slot(proc_mem_t* mem, uint32_t size, uint32_t* out_vaddr) {
    if (!mem || !out_vaddr) {
        return 0;
    }

    kernel::RwSpinLockNativeWriteGuard guard(mem->mmap_lock);

    if (size == 0u) {
        return 0;
    }

    const uint32_t aligned_size = align_up_4k(size);

    const uint32_t mmap_base = 0xB0000000u;
    uint32_t search_top = mem->free_area_cache;
    if (search_top == 0u || search_top > mmap_base) {
        search_top = mmap_base;
    }

    uint32_t floor = user_addr_min;
    if (mem->heap_start < mem->prog_break) {
        uint64_t need64 = (uint64_t)mem->prog_break + 0x100000ull;
        if (need64 < user_addr_max) {
            floor = align_up_4k((uint32_t)need64);
        }
    }

    if (floor >= mmap_base || aligned_size == 0u) {
        mem->free_area_cache = mmap_base;
        return 0;
    }

    auto subtree_can_fit = [&](const vma_region_t* region, uint32_t pred_end, uint32_t succ_start) -> bool {
        if (!region) {
            uint32_t base = (pred_end > floor) ? pred_end : floor;
            return succ_start >= base + aligned_size;
        }

        if (succ_start < floor + aligned_size) {
            return false;
        }

        uint32_t best = region->subtree_max_gap;

        uint32_t right_gap = 0u;
        if (region->subtree_max_end < succ_start) {
            right_gap = succ_start - region->subtree_max_end;
        }
        if (right_gap > best) {
            best = right_gap;
        }

        uint32_t base = (pred_end > floor) ? pred_end : floor;
        uint32_t left_gap = 0u;
        if (base < region->subtree_min_start) {
            left_gap = region->subtree_min_start - base;
        }
        if (left_gap > best) {
            best = left_gap;
        }

        return best >= aligned_size;
    };

    auto try_top_down = [&](uint32_t top) -> uint32_t {
        if (top > mmap_base) {
            top = mmap_base;
        }

        if (top < floor + aligned_size) {
            return 0u;
        }

        uint32_t limit = align_down_4k(top);

        if (!subtree_can_fit(rb_to_region(mem->mmap_tree.rb_node), floor, limit)) {
            return 0u;
        }

        struct frame_t {
            rb_node* node;
            uint32_t pred_end;
            uint32_t succ_start;
            uint8_t state;
        };

        frame_t stack[64] = {};
        uint32_t sp = 0u;

        stack[sp++] = frame_t{mem->mmap_tree.rb_node, floor, limit, 0u};

        while (sp != 0u) {
            frame_t& f = stack[sp - 1u];

            if (!f.node) {
                const uint32_t base = (f.pred_end > floor) ? f.pred_end : floor;
                if (f.succ_start >= base + aligned_size) {
                    const uint32_t start = align_down_4k(f.succ_start - aligned_size);
                    if (start >= base) {
                        return start;
                    }
                }

                sp--;
                continue;
            }

            vma_region_t* cur = rb_to_region(f.node);
            if (!cur) {
                sp--;
                continue;
            }

            if (cur->vaddr_start >= f.succ_start) {
                f.node = f.node->rb_left;
                f.state = 0u;
                continue;
            }

            if (f.state == 0u) {
                rb_node* right_node = f.node->rb_right;
                if (right_node) {
                    vma_region_t* right = rb_to_region(right_node);
                    if (right && subtree_can_fit(right, cur->vaddr_end, f.succ_start)) {
                        f.state = 1u;
                        if (sp < 64u) {
                            stack[sp++] = frame_t{right_node, cur->vaddr_end, f.succ_start, 0u};
                            continue;
                        }
                    }
                }

                f.state = 1u;
            }

            if (f.state == 1u) {
                const uint32_t gap_end = f.succ_start;
                uint32_t gap_base = cur->vaddr_end;
                if (gap_base < floor) {
                    gap_base = floor;
                }

                if (gap_end >= gap_base + aligned_size) {
                    const uint32_t start = align_down_4k(gap_end - aligned_size);
                    if (start >= gap_base) {
                        return start;
                    }
                }

                f.state = 2u;
            }

            if (f.state == 2u) {
                rb_node* left_node = f.node->rb_left;
                if (left_node) {
                    vma_region_t* left = rb_to_region(left_node);
                    if (left && subtree_can_fit(left, f.pred_end, cur->vaddr_start)) {
                        f.state = 3u;
                        if (sp < 64u) {
                            stack[sp++] = frame_t{left_node, f.pred_end, cur->vaddr_start, 0u};
                            continue;
                        }
                    }
                }

                f.state = 3u;
            }

            sp--;
        }

        return 0u;
    };

    uint32_t start = try_top_down(search_top);
    if (start == 0u && search_top != mmap_base) {
        start = try_top_down(mmap_base);
    }

    if (start == 0u) {
        mem->free_area_cache = mmap_base;
        return 0;
    }

    uint32_t end_excl = start + aligned_size;
    if (end_excl < start) {
        mem->free_area_cache = mmap_base;
        return 0;
    }

    *out_vaddr = start;
    mem->free_area_cache = start;

    if (end_excl > mem->mmap_top) {
        mem->mmap_top = end_excl;
    }

    return 1;
}

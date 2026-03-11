// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <mm/vma.h>

#include <arch/i386/paging.h>

#include <fs/vfs.h>

#include <kernel/proc.h>

#include <mm/pmm.h>

#include <lib/cpp/new.h>

#include <lib/cpp/lock_guard.h>

#include <string.h>

extern "C" void smp_tlb_shootdown_range(uint32_t start, uint32_t end);

namespace {

constexpr uint32_t page_size = 4096u;
constexpr uint32_t page_mask = 4095u;

constexpr uint32_t user_addr_min = 0x08000000u;
constexpr uint32_t user_addr_max = 0xC0000000u;

constexpr uint32_t user_stack_addr_min = 0x60000000u;
constexpr uint32_t user_stack_addr_max = 0x80000000u;

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

static void vma_region_recalc(vma_region_t* region) noexcept {
    if (!region) {
        return;
    }

    vma_region_t* left = rb_to_region(region->rb_node.rb_left);
    vma_region_t* right = rb_to_region(region->rb_node.rb_right);

    uint32_t min_start = region->vaddr_start;
    uint32_t max_end = region->vaddr_end;
    uint32_t max_gap = 0u;

    if (left) {
        if (left->subtree_min_start < min_start) {
            min_start = left->subtree_min_start;
        }

        if (left->subtree_max_end > max_end) {
            max_end = left->subtree_max_end;
        }

        if (left->subtree_max_gap > max_gap) {
            max_gap = left->subtree_max_gap;
        }

        if (left->subtree_max_end < region->vaddr_start) {
            uint32_t gap = region->vaddr_start - left->subtree_max_end;
            if (gap > max_gap) {
                max_gap = gap;
            }
        }
    }

    if (right) {
        if (right->subtree_min_start < min_start) {
            min_start = right->subtree_min_start;
        }

        if (right->subtree_max_end > max_end) {
            max_end = right->subtree_max_end;
        }

        if (right->subtree_max_gap > max_gap) {
            max_gap = right->subtree_max_gap;
        }

        if (region->vaddr_end < right->subtree_min_start) {
            uint32_t gap = right->subtree_min_start - region->vaddr_end;
            if (gap > max_gap) {
                max_gap = gap;
            }
        }
    }

    region->subtree_min_start = min_start;
    region->subtree_max_end = max_end;
    region->subtree_max_gap = max_gap;
}

static void vma_region_propagate(rb_node* node, rb_node* stop) {
    for (rb_node* cur = node; cur && cur != stop; cur = rb_parent(cur)) {
        vma_region_recalc(rb_to_region(cur));
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
    vma_region_recalc(rb_to_region(old));
    vma_region_recalc(rb_to_region(new_node));
}

static const rb_augment_callbacks vma_rb_callbacks = {
    .propagate = vma_region_propagate,
    .copy = vma_region_copy,
    .rotate = vma_region_rotate,
};

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

[[maybe_unused]] static vma_region_t* tree_first_ge(proc_mem_t* mem, uint32_t vaddr) noexcept {
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

        if (cur->vaddr_start < vaddr) {
            node = node->rb_right;
            continue;
        }

        best = cur;
        node = node->rb_left;
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

    rb_erase_augmented(&region->rb_node, &mem->mmap_tree, &vma_rb_callbacks);
    region->rb_node.__parent_color = 0;
    region->rb_node.rb_left = nullptr;
    region->rb_node.rb_right = nullptr;
}

static vma_region_t* vma_find_unlocked(proc_mem_t* mem, uint32_t vaddr) noexcept {
    vma_region_t* cand = tree_find_leq(mem, vaddr);
    if (!cand) {
        return nullptr;
    }

    if (vaddr >= cand->vaddr_start && vaddr < cand->vaddr_end) {
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

    for (uint32_t v = start; v < end; v += page_size) {
        uint32_t pte;

        if (!paging_get_present_pte(page_dir, v, &pte)) {
            continue;
        }

        if ((pte & 4u) == 0u) {
            continue;
        }

        paging_map_ex(page_dir, v, 0u, 0u, PAGING_MAP_NO_TLB_FLUSH);

        uint32_t phys = pte & ~page_mask;

        if ((pte & 0x200u) == 0u) {
            pmm_free_block((void*)phys);
        }
    }

    smp_tlb_shootdown_range(aligned_start, aligned_end);
}

static void release_region_file(vma_region_t* region) noexcept {
    if (region && region->file) {
        vfs_node_release(region->file);
        region->file = nullptr;
    }
}

static vma_region_t* alloc_region() noexcept {
    return new (kernel::nothrow) vma_region_t{};
}

static void free_region(vma_region_t* region) noexcept {
    if (region) {
        release_region_file(region);
        delete region;
    }
}

}

extern "C" void vma_init(proc_mem_t* mem) {
    if (!mem) {
        return;
    }

    rwspinlock_init(&mem->mmap_lock);

    mem->mmap_tree = RB_ROOT;
    dlist_init(&mem->mmap_regions);

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

    while (true) {
        vma_region_t* victim = nullptr;

        {
            kernel::RwSpinLockNativeWriteSafeGuard guard(mem->mmap_lock);

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

    vma_region_t* region = alloc_region();
    if (!region) {
        return nullptr;
    }

    region->vaddr_start = aligned_vaddr;
    region->vaddr_end = aligned_vaddr + aligned_size;
    region->file_offset = file_offset - diff;
    region->length = size;
    region->file_size = aligned_file_size;
    region->map_flags = flags;
    region->file = nullptr;

    region->subtree_min_start = region->vaddr_start;
    region->subtree_max_end = region->vaddr_end;
    region->subtree_max_gap = 0u;

    region->rb_node.__parent_color = 0;
    region->rb_node.rb_left = nullptr;
    region->rb_node.rb_right = nullptr;
    region->list_node.next = nullptr;
    region->list_node.prev = nullptr;

    if (file) {
        vfs_node_retain(file);
        region->file = file;
    }

    bool has_overlap = false;

    {
        kernel::RwSpinLockNativeWriteSafeGuard guard(mem->mmap_lock);

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

    return region;
}

extern "C" vma_region_t* vma_find(proc_mem_t* mem, uint32_t vaddr) {
    if (!mem) {
        return nullptr;
    }

    kernel::RwSpinLockNativeReadSafeGuard guard(mem->mmap_lock);
    return vma_find_unlocked(mem, vaddr);
}

extern "C" vma_region_t* vma_find_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem) {
        return nullptr;
    }

    kernel::RwSpinLockNativeReadSafeGuard guard(mem->mmap_lock);
    return vma_find_overlap_unlocked(mem, start, end_excl);
}

extern "C" int vma_has_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    return vma_find_overlap(mem, start, end_excl) != nullptr ? 1 : 0;
}

extern "C" int vma_remove(proc_mem_t* mem, uint32_t vaddr, uint32_t len) {
    if (!mem || !mem->page_dir) {
        return -1;
    }

    struct unmap_span_t {
        uint32_t start;
        uint32_t end;
    };

    unmap_span_t* spans = nullptr;
    uint32_t spans_cap = 0u;
    uint32_t spans_len = 0u;

    auto cleanup_spans = [&]() {
        delete[] spans;
        spans = nullptr;
        spans_cap = 0u;
        spans_len = 0u;
    };

    if (vaddr & page_mask) {
        return -1;
    }

    if (len == 0u) {
        return -1;
    }

    if (vaddr + len < vaddr) {
        return -1;
    }

    uint32_t aligned_len = align_up_4k(len);
    uint32_t vaddr_end = vaddr + aligned_len;

    {
        kernel::RwSpinLockNativeWriteSafeGuard guard(mem->mmap_lock);

        uint32_t need_spans = 0u;

        uint32_t scan = vaddr;
        while (scan < vaddr_end) {
            vma_region_t* region = vma_find_unlocked(mem, scan);
            if (!region || region->vaddr_end <= scan) {
                return -1;
            }

            need_spans++;
            scan = region->vaddr_end;
        }

        if (need_spans == 0u) {
            return 0;
        }

        spans = new (kernel::nothrow) unmap_span_t[need_spans]{};
        if (!spans) {
            return -1;
        }

        spans_cap = need_spans;

        auto push_span = [&](uint32_t start, uint32_t end) {
            if (start >= end) {
                return;
            }

            if (spans_len != 0u) {
                unmap_span_t& last = spans[spans_len - 1u];
                if (start <= last.end) {
                    if (end > last.end) {
                        last.end = end;
                    }
                    return;
                }
            }

            if (spans_len < spans_cap) {
                spans[spans_len++] = unmap_span_t{start, end};
            }
        };

        scan = vaddr;
        while (scan < vaddr_end) {
            vma_region_t* curr = vma_find_unlocked(mem, scan);
            if (!curr) {
                cleanup_spans();
                return -1;
            }

            const uint32_t u_start = vaddr;
            const uint32_t u_end = vaddr_end;
            const uint32_t m_start = curr->vaddr_start;
            const uint32_t m_end = curr->vaddr_end;

            const uint32_t o_start = (u_start > m_start) ? u_start : m_start;
            const uint32_t o_end = (u_end < m_end) ? u_end : m_end;

            if (o_start >= o_end) {
                scan = m_end;
                continue;
            }

            push_span(o_start, o_end);

            if (o_start > m_start && o_end < m_end) {
                vma_region_t* new_right = alloc_region();
                if (!new_right) {
                    cleanup_spans();
                    return -1;
                }

                const uint32_t orig_file_size = curr->file_size;
                const uint32_t left_len = o_start - m_start;
                const uint32_t right_len = m_end - o_end;
                const uint32_t cut_before_right = o_end - m_start;

                new_right->vaddr_start = o_end;
                new_right->vaddr_end = m_end;
                new_right->length = right_len;
                new_right->map_flags = curr->map_flags;

                new_right->subtree_min_start = new_right->vaddr_start;
                new_right->subtree_max_end = new_right->vaddr_end;
                new_right->subtree_max_gap = 0u;

                new_right->rb_node.__parent_color = 0;
                new_right->rb_node.rb_left = nullptr;
                new_right->rb_node.rb_right = nullptr;
                new_right->list_node.next = nullptr;
                new_right->list_node.prev = nullptr;

                if (curr->file) {
                    vfs_node_retain(curr->file);
                    new_right->file = curr->file;
                    new_right->file_offset = curr->file_offset + cut_before_right;
                } else {
                    new_right->file = nullptr;
                    new_right->file_offset = 0u;
                }

                uint32_t right_file_size = 0u;
                if (orig_file_size > cut_before_right) {
                    right_file_size = orig_file_size - cut_before_right;
                }
                if (right_file_size > right_len) {
                    right_file_size = right_len;
                }
                new_right->file_size = right_file_size;

                curr->vaddr_end = o_start;
                curr->length = left_len;

                uint32_t left_file_size = orig_file_size;
                if (left_file_size > left_len) {
                    left_file_size = left_len;
                }
                curr->file_size = left_file_size;

                if (!tree_insert(mem, new_right)) {
                    free_region(new_right);
                    cleanup_spans();
                    return -1;
                }

                dlist_add_tail(&new_right->list_node, &mem->mmap_regions);
                vma_region_propagate(&curr->rb_node, nullptr);

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
                const uint32_t cut_len = o_end - m_start;
                const uint32_t new_len = m_end - o_end;

                tree_erase(mem, curr);

                curr->vaddr_start = o_end;
                curr->vaddr_end = m_end;
                curr->length = new_len;

                if (curr->file) {
                    curr->file_offset += cut_len;
                }

                if (curr->file_size > cut_len) {
                    curr->file_size -= cut_len;
                } else {
                    curr->file_size = 0u;
                }

                if (curr->file_size > new_len) {
                    curr->file_size = new_len;
                }

                if (!tree_insert(mem, curr)) {
                    cleanup_spans();
                    return -1;
                }

                scan = o_end;
                continue;
            }

            if (o_start > m_start && o_end == m_end) {
                const uint32_t new_len = o_start - m_start;

                curr->vaddr_end = o_start;
                curr->length = new_len;

                if (curr->file_size > new_len) {
                    curr->file_size = new_len;
                }

                vma_region_propagate(&curr->rb_node, nullptr);

                scan = o_end;
                continue;
            }

            scan = o_end;
        }
    }

    for (uint32_t i = 0u; i < spans_len; i++) {
        unmap_range(mem->page_dir, spans[i].start, spans[i].end);
    }

    cleanup_spans();
    return 0;
}

extern "C" int vma_validate_range(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem || !mem->page_dir) {
        return 0;
    }

    kernel::RwSpinLockNativeReadSafeGuard guard(mem->mmap_lock);

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

    kernel::RwSpinLockNativeWriteSafeGuard guard(mem->mmap_lock);

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

        auto find_slot_rec = [&](auto&& self, rb_node* node, uint32_t pred_end, uint32_t succ_start) -> uint32_t {
            if (!node) {
                uint32_t base = (pred_end > floor) ? pred_end : floor;
                if (succ_start < base + aligned_size) {
                    return 0u;
                }

                uint32_t start = align_down_4k(succ_start - aligned_size);
                return (start >= base) ? start : 0u;
            }

            vma_region_t* cur = rb_to_region(node);
            if (!cur) {
                return 0u;
            }

            if (cur->vaddr_start >= succ_start) {
                return self(self, node->rb_left, pred_end, succ_start);
            }

            if (rb_node* right_node = node->rb_right) {
                vma_region_t* right = rb_to_region(right_node);
                if (right && subtree_can_fit(right, cur->vaddr_end, succ_start)) {
                    if (uint32_t found = self(self, right_node, cur->vaddr_end, succ_start)) {
                        return found;
                    }
                }
            }

            uint32_t gap_end = succ_start;
            uint32_t gap_base = cur->vaddr_end;
            if (gap_base < floor) {
                gap_base = floor;
            }

            if (gap_end >= gap_base + aligned_size) {
                uint32_t start = align_down_4k(gap_end - aligned_size);
                if (start >= gap_base) {
                    return start;
                }
            }

            if (rb_node* left_node = node->rb_left) {
                vma_region_t* left = rb_to_region(left_node);
                if (left && subtree_can_fit(left, pred_end, cur->vaddr_start)) {
                    if (uint32_t found = self(self, left_node, pred_end, cur->vaddr_start)) {
                        return found;
                    }
                }
            }

            return 0u;
        };

        if (!subtree_can_fit(rb_to_region(mem->mmap_tree.rb_node), floor, limit)) {
            return 0u;
        }

        return find_slot_rec(find_slot_rec, mem->mmap_tree.rb_node, floor, limit);
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

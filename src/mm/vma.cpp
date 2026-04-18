/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <mm/heap.h>
#include <mm/vma.h>
#include <mm/pmm.h>

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>

#include <arch/i386/paging.h>

#include <kernel/proc.h>

#include <fs/vfs.h>

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

static inline uint32_t region_span(const vma_region_t* region) noexcept {
    return region ? (region->vaddr_end - region->vaddr_start) : 0u;
}

static void free_region(vma_region_t* region) noexcept;

static void free_region_rcu_cb(rcu_head_t* head) noexcept {
    if (!head) {
        return;
    }

    vma_region_t* region = container_of(head, vma_region_t, rcu);
    free_region(region);
}

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

    r->rcu = rcu_head_t{};

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

static void mt_insert_region(proc_mem_t* mem, vma_region_t* region) noexcept {
    mt_store(&mem->mmap_mt, region->vaddr_start, region->vaddr_end - 1u, region);
}

static void mt_erase_region(proc_mem_t* mem, vma_region_t* region) noexcept {
    mt_erase(&mem->mmap_mt, region->vaddr_start, region->vaddr_end - 1u);
}

___inline void vmacache_invalidate(proc_mem_t* mem) noexcept {
    if (mem) {
        __atomic_fetch_add(&mem->vmacache_seq, 1, __ATOMIC_RELEASE);
    }
}

static vma_region_t* vmacache_find(task_t* t, proc_mem_t* mem, uint32_t vaddr) noexcept {
    if (!t
        || !mem) {
        return nullptr;
    }

    uint32_t seq = __atomic_load_n(&mem->vmacache_seq, __ATOMIC_ACQUIRE);
    if (t->vmacache_seq != seq) {
        return nullptr;
    }

    for (int i = 0; i < 4; i++) {
        vma_region_t* vma = t->vmacache[i];

        if (vma && vaddr >= vma->vaddr_start && vaddr < vma->vaddr_end) {
            return vma;
        }
    }
    return nullptr;
}

static void vmacache_update(task_t* t, proc_mem_t* mem, vma_region_t* vma) noexcept {
    if (!t
        || !mem
        || !vma) {
        return;
    }

    uint32_t seq = __atomic_load_n(&mem->vmacache_seq, __ATOMIC_ACQUIRE);

    if (t->vmacache_seq != seq) {
        t->vmacache[0] = nullptr; t->vmacache[1] = nullptr;
        t->vmacache[2] = nullptr; t->vmacache[3] = nullptr;

        t->vmacache_seq = seq;
    }

    if (t->vmacache[0] == vma) {
        return;
    }

    if (t->vmacache[1] == vma) {
        t->vmacache[1] = t->vmacache[0];
        t->vmacache[0] = vma; return; 
    }
    
    if (t->vmacache[2] == vma) {
        t->vmacache[2] = t->vmacache[1];
        t->vmacache[1] = t->vmacache[0];
        t->vmacache[0] = vma; return;
    }
    
    t->vmacache[3] = t->vmacache[2];
    t->vmacache[2] = t->vmacache[1];
    t->vmacache[1] = t->vmacache[0];
    t->vmacache[0] = vma;
}

static void merge_regions_into_left(proc_mem_t* mem, vma_region_t* left, vma_region_t* right) noexcept {
    if (!mem || !left || !right) {
        return;
    }

    uint32_t left_span = region_span(left);
    uint32_t right_span = region_span(right);
    uint32_t merged_span = left_span + right_span;

    mt_erase_region(mem, left);
    mt_erase_region(mem, right);

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

    mt_insert_region(mem, left);

    vmacache_invalidate(mem);

    call_rcu(&right->rcu, free_region_rcu_cb);
}

static vma_region_t* vma_find_lockless(task_t* t, proc_mem_t* mem, uint32_t vaddr) noexcept {
    if (!mem) {
        return nullptr;
    }

    if (t) {
        vma_region_t* cached = vmacache_find(t, mem, vaddr);
        
        if (cached) {
            return cached;
        }
    }

    vma_region_t* cand = static_cast<vma_region_t*>(mt_load(&mem->mmap_mt, vaddr));

    if (cand && vaddr >= cand->vaddr_start && vaddr < cand->vaddr_end) {
        if (t) {
            vmacache_update(t, mem, cand);
        }

        return cand;
    }

    return nullptr;
}

static vma_region_t* vma_find_overlap_unlocked(proc_mem_t* mem, uint32_t start, uint32_t end_excl) noexcept {
    if (!mem || end_excl <= start) {
        return nullptr;
    }

    vma_region_t* cand = static_cast<vma_region_t*>(mt_load(&mem->mmap_mt, start));

    if (cand && ranges_overlap(start, end_excl, cand->vaddr_start, cand->vaddr_end)) {
        return cand;
    }

    if (cand) {
        uint32_t next_idx = cand->vaddr_end;
        vma_region_t* next = static_cast<vma_region_t*>(mt_find_after(&mem->mmap_mt, next_idx));

        if (next && ranges_overlap(start, end_excl, next->vaddr_start, next->vaddr_end)) {
            return next;
        }
    } else {
        vma_region_t* first = static_cast<vma_region_t*>(mt_find_after(&mem->mmap_mt, start));

        if (first && ranges_overlap(start, end_excl, first->vaddr_start, first->vaddr_end)) {
            return first;
        }
    }

    return nullptr;
}

static void unmap_range(uint32_t* page_dir, uint32_t start, uint32_t end) noexcept {
    const uint32_t aligned_start = align_down_4k(start);
    const uint32_t aligned_end = align_up_4k(end);

    struct UnmapCtx {
        uint32_t freed_pages = 0u;
    } ctx{};

    auto visitor = [](uint32_t /*virt*/, uint32_t pte, void* vctx) -> int {
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

    mt_init_cache();

    spinlock_init(&mem->mmap_lock);

    mt_init(&mem->mmap_mt);

    __atomic_store_n(&mem->vmacache_seq, 0, __ATOMIC_RELAXED);

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

    kernel::SpinLockNativeGuard guard(mem->mmap_lock);

    uint32_t idx = 0u;
    while (true) {
        vma_region_t* region = static_cast<vma_region_t*>(mt_find_after(&mem->mmap_mt, idx));
        if (!region) {
            break;
        }

        mt_erase_region(mem, region);
        free_region(region);

        idx = 0u;
    }

    mt_destroy(&mem->mmap_mt);
}

extern "C" vma_region_t* vma_create(
    proc_mem_t* mem, uint32_t vaddr, uint32_t size,
    vfs_node_t* file, uint32_t file_offset, uint32_t file_size,
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
        kernel::SpinLockNativeGuard guard(mem->mmap_lock);

        has_overlap = vma_find_overlap_unlocked(mem, region->vaddr_start, region->vaddr_end) != nullptr;

        if (!has_overlap) {
            mt_insert_region(mem, region);

            vma_region_t* merged = region;

            uint32_t prev_idx = merged->vaddr_start;

            if (mt_prev(&mem->mmap_mt, &prev_idx)) {
                vma_region_t* prev = static_cast<vma_region_t*>(mt_load(&mem->mmap_mt, prev_idx));
                
                if (prev && regions_are_mergeable(prev, merged)) {
                    merge_regions_into_left(mem, prev, merged);

                    merged = prev;
                }
            }

            while (true) {
                uint32_t next_idx = merged->vaddr_end;

                if (!mt_next(&mem->mmap_mt, &next_idx)) {
                    break;
                }

                vma_region_t* next = static_cast<vma_region_t*>(mt_load(&mem->mmap_mt, next_idx));
                
                if (!next || !regions_are_mergeable(merged, next)) {
                    break;
                }

                merge_regions_into_left(mem, merged, next);
            }

            region = merged;

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

extern "C" vma_region_t* vma_find(task_t* t, proc_mem_t* mem, uint32_t vaddr) {
    return vma_find_lockless(t, mem, vaddr);
}

extern "C" vma_region_t* vma_find_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    kernel::RcuReadGuard guard;

    return vma_find_overlap_unlocked(mem, start, end_excl);
}

extern "C" int vma_has_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    return vma_find_overlap(mem, start, end_excl) != nullptr ? 1 : 0;
}

extern "C" int vma_remove(proc_mem_t* mem, uint32_t vaddr, uint32_t len) {
    if (!mem || !mem->page_dir || (vaddr & page_mask)
        || len == 0u || vaddr + len < vaddr) {
        return -1;
    }

    uint32_t vaddr_end = vaddr + align_up_4k(len);
    
    UnmapSpanCollector collector;

    {
        kernel::SpinLockNativeGuard guard(mem->mmap_lock);

        uint32_t need_spans = 0u;
        uint32_t scan = vaddr;

        while (scan < vaddr_end) {
            vma_region_t* r = vma_find_lockless(nullptr, mem, scan);

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
            vma_region_t* curr = vma_find_lockless(nullptr, mem, scan);

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

                mt_erase_region(mem, curr);

                curr->vaddr_end = o_start;
                curr->length = o_start - m_start;

                adjust_file_bounds(curr, 0u, curr->length, false);

                mt_insert_region(mem, curr);
                mt_insert_region(mem, new_right);

                scan = o_end;
                continue;
            }

            if (o_start == m_start && o_end == m_end) {
                mt_erase_region(mem, curr);
                
                vmacache_invalidate(mem);

                call_rcu(&curr->rcu, free_region_rcu_cb);
                
                scan = o_end;
                continue;
            }

            if (o_start == m_start && o_end < m_end) {
                mt_erase_region(mem, curr);

                curr->vaddr_start = o_end;
                curr->length = m_end - o_end;

                adjust_file_bounds(curr, o_end - m_start, curr->length, true);

                mt_insert_region(mem, curr);

                scan = o_end;
                continue;
            }

            if (o_start > m_start && o_end == m_end) {
                mt_erase_region(mem, curr);

                curr->vaddr_end = o_start;
                curr->length = o_start - m_start;

                adjust_file_bounds(curr, 0u, curr->length, false);

                mt_insert_region(mem, curr);

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

    kernel::RcuReadGuard guard;

    if (end_excl <= start) {
        return 1;
    }

    if (start < user_addr_min || end_excl > user_addr_max) {
        return 0;
    }

    uint32_t cur = start;

    while (cur < end_excl) {

        vma_region_t* region = vma_find_lockless(nullptr, mem, cur);

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

    kernel::SpinLockNativeGuard guard(mem->mmap_lock);

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

    uint32_t found_addr = 0u;

    if (mt_gap_find(&mem->mmap_mt, aligned_size, &found_addr, floor, search_top)) {
        if (found_addr >= floor && found_addr + aligned_size <= search_top) {
            *out_vaddr = found_addr;
            mem->free_area_cache = found_addr;

            uint32_t end_excl = found_addr + aligned_size;

            if (end_excl > mem->mmap_top) {
                mem->mmap_top = end_excl;
            }

            return 1;
        }
    }

    if (search_top != mmap_base) {
        if (mt_gap_find(&mem->mmap_mt, aligned_size, &found_addr, floor, mmap_base)) {
            if (found_addr >= floor && found_addr + aligned_size <= mmap_base) {
                *out_vaddr = found_addr;
                mem->free_area_cache = found_addr;

                uint32_t end_excl = found_addr + aligned_size;

                if (end_excl > mem->mmap_top) {
                    mem->mmap_top = end_excl;
                }

                return 1;
            }
        }
    }

    mem->free_area_cache = mmap_base;
    return 0;
}

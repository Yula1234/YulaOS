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

static vma_region_t* tree_first_ge(proc_mem_t* mem, uint32_t vaddr) noexcept {
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
    rb_insert_color(&region->rb_node, &mem->mmap_tree);
    return 1;
}

static void tree_erase(proc_mem_t* mem, vma_region_t* region) noexcept {
    if (!mem || !region) {
        return;
    }

    rb_erase(&region->rb_node, &mem->mmap_tree);
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
    for (uint32_t v = start; v < end; v += page_size) {
        uint32_t pte;

        if (!paging_get_present_pte(page_dir, v, &pte)) {
            continue;
        }

        if ((pte & 4u) == 0u) {
            continue;
        }

        paging_map(page_dir, v, 0, 0);

        uint32_t phys = pte & ~page_mask;

        if ((pte & 0x200u) == 0u) {
            pmm_free_block((void*)phys);
        }
    }
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

    kernel::RwSpinLockNativeWriteSafeGuard guard(mem->mmap_lock);

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

    uint32_t scan = vaddr;
    while (scan < vaddr_end) {
        vma_region_t* region = vma_find_unlocked(mem, scan);

        if (!region || region->vaddr_end <= scan) {
            return -1;
        }

        scan = region->vaddr_end;
    }

    scan = vaddr;
    while (scan < vaddr_end) {
        vma_region_t* curr = vma_find_unlocked(mem, scan);
        if (!curr) {
            return -1;
        }

        uint32_t u_start = vaddr;
        uint32_t u_end = vaddr_end;
        uint32_t m_start = curr->vaddr_start;
        uint32_t m_end = curr->vaddr_end;

        uint32_t o_start = (u_start > m_start) ? u_start : m_start;
        uint32_t o_end = (u_end < m_end) ? u_end : m_end;

        if (o_start >= o_end) {
            scan = m_end;
            continue;
        }

        if (o_start > m_start && o_end < m_end) {
            vma_region_t* new_right = alloc_region();
            if (!new_right) {
                return -1;
            }

            uint32_t orig_file_size = curr->file_size;
            uint32_t left_len = o_start - m_start;
            uint32_t right_len = m_end - o_end;
            uint32_t cut_before_right = o_end - m_start;

            new_right->vaddr_start = o_end;
            new_right->vaddr_end = m_end;
            new_right->length = right_len;
            new_right->map_flags = curr->map_flags;

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
                return -1;
            }

            dlist_add_tail(&new_right->list_node, &mem->mmap_regions);

            unmap_range(mem->page_dir, o_start, o_end);
            scan = o_end;
            continue;
        }

        unmap_range(mem->page_dir, o_start, o_end);

        if (o_start == m_start && o_end == m_end) {
            dlist_del(&curr->list_node);
            tree_erase(mem, curr);
            free_region(curr);
            scan = o_end;
            continue;
        }

        if (o_start == m_start && o_end < m_end) {
            uint32_t cut_len = o_end - m_start;
            uint32_t new_len = m_end - o_end;

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
                return -1;
            }

            scan = o_end;
            continue;
        }

        if (o_start > m_start && o_end == m_end) {
            uint32_t new_len = o_start - m_start;

            curr->vaddr_end = o_start;
            curr->length = new_len;

            if (curr->file_size > new_len) {
                curr->file_size = new_len;
            }

            scan = o_end;
            continue;
        }

        scan = o_end;
    }

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

    uint32_t aligned_size = align_up_4k(size);

    uint32_t vaddr = align_up_4k(0x80000000u);

    if (mem->heap_start < mem->prog_break) {
        uint64_t need64 = (uint64_t)mem->prog_break + 0x100000ull;

        if (need64 < user_addr_max) {
            uint32_t need = align_up_4k((uint32_t)need64);

            if (vaddr < need) {
                vaddr = need;
            }
        }
    }

    const uint32_t alloc_limit = 0xB0000000u;

    if (vaddr >= alloc_limit) {
        return 0;
    }

    uint32_t cur = vaddr;

    vma_region_t* left = tree_find_leq(mem, cur);
    if (left && left->vaddr_end > cur) {
        cur = align_up_4k(left->vaddr_end);
    }

    vma_region_t* right = tree_first_ge(mem, cur);

    while (cur < alloc_limit) {
        uint32_t gap_end = alloc_limit;

        if (right) {
            gap_end = right->vaddr_start;
        }

        uint32_t end_excl = cur + aligned_size;

        if (end_excl < cur) {
            return 0;
        }

        if (end_excl <= gap_end) {
            *out_vaddr = cur;

            if (end_excl > mem->mmap_top) {
                mem->mmap_top = end_excl;
            }

            return 1;
        }

        if (!right) {
            return 0;
        }

        if (right->vaddr_end > cur) {
            cur = align_up_4k(right->vaddr_end);
        }

        right = rb_to_region(rb_next(&right->rb_node));
    }

    return 0;
}

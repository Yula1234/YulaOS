// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <mm/vma.h>

#include <arch/i386/paging.h>

#include <fs/vfs.h>

#include <kernel/proc.h>

#include <mm/pmm.h>

#include <lib/cpp/new.h>

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

static int paging_get_present_pte(uint32_t* dir, uint32_t virt, uint32_t* out_pte) noexcept {
    if (!dir) {
        return 0;
    }

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FFu;

    uint32_t pde = dir[pd_idx];
    if ((pde & 1u) == 0u) {
        return 0;
    }

    uint32_t* pt = (uint32_t*)(pde & ~page_mask);
    uint32_t pte = pt[pt_idx];
    if ((pte & 1u) == 0u) {
        return 0;
    }

    if (out_pte) {
        *out_pte = pte;
    }

    return 1;
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

    mem->mmap_list = nullptr;
    mem->mmap_top = user_addr_min;
}

extern "C" void vma_destroy(proc_mem_t* mem) {
    if (!mem) {
        return;
    }

    vma_region_t* curr = mem->mmap_list;

    while (curr) {
        vma_region_t* next = curr->next;

        free_region(curr);

        curr = next;
    }

    mem->mmap_list = nullptr;
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

    if (file) {
        vfs_node_retain(file);
        region->file = file;
    }

    region->next = mem->mmap_list;
    mem->mmap_list = region;

    return region;
}

extern "C" vma_region_t* vma_find(proc_mem_t* mem, uint32_t vaddr) {
    if (!mem) {
        return nullptr;
    }

    vma_region_t* curr = mem->mmap_list;

    while (curr) {
        if (vaddr >= curr->vaddr_start && vaddr < curr->vaddr_end) {
            return curr;
        }

        curr = curr->next;
    }

    return nullptr;
}

extern "C" vma_region_t* vma_find_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem) {
        return nullptr;
    }

    vma_region_t* curr = mem->mmap_list;

    while (curr) {
        if (ranges_overlap(start, end_excl, curr->vaddr_start, curr->vaddr_end)) {
            return curr;
        }

        curr = curr->next;
    }

    return nullptr;
}

extern "C" int vma_has_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    return vma_find_overlap(mem, start, end_excl) != nullptr ? 1 : 0;
}

extern "C" int vma_remove(proc_mem_t* mem, uint32_t vaddr, uint32_t len) {
    if (!mem || !mem->page_dir) {
        return -1;
    }

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
        vma_region_t* region = vma_find(mem, scan);

        if (!region || region->vaddr_end <= scan) {
            return -1;
        }

        scan = region->vaddr_end;
    }

    vma_region_t* prev = nullptr;
    vma_region_t* curr = mem->mmap_list;

    while (curr) {
        vma_region_t* next = curr->next;

        uint32_t u_start = vaddr;
        uint32_t u_end = vaddr_end;
        uint32_t m_start = curr->vaddr_start;
        uint32_t m_end = curr->vaddr_end;

        if (u_end <= m_start || u_start >= m_end) {
            prev = curr;
            curr = next;
            continue;
        }

        uint32_t o_start = (u_start > m_start) ? u_start : m_start;
        uint32_t o_end = (u_end < m_end) ? u_end : m_end;

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
            new_right->next = curr->next;

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

            curr->next = new_right;

            unmap_range(mem->page_dir, o_start, o_end);

            prev = new_right;
            curr = new_right->next;
            continue;
        }

        unmap_range(mem->page_dir, o_start, o_end);

        if (o_start == m_start && o_end == m_end) {
            if (prev) {
                prev->next = next;
            } else {
                mem->mmap_list = next;
            }

            free_region(curr);

            curr = next;
            continue;
        }

        if (o_start == m_start && o_end < m_end) {
            uint32_t cut_len = o_end - m_start;
            uint32_t new_len = m_end - o_end;

            curr->vaddr_start = o_end;
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

            prev = curr;
            curr = next;
            continue;
        }

        if (o_start > m_start && o_end == m_end) {
            uint32_t new_len = o_start - m_start;

            curr->vaddr_end = o_start;
            curr->length = new_len;

            if (curr->file_size > new_len) {
                curr->file_size = new_len;
            }

            prev = curr;
            curr = next;
            continue;
        }

        prev = curr;
        curr = next;
    }

    return 0;
}

extern "C" int vma_validate_range(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem || !mem->page_dir) {
        return 0;
    }

    if (end_excl <= start) {
        return 1;
    }

    if (start < user_addr_min || end_excl > user_addr_max) {
        return 0;
    }

    uint32_t cur = start;

    while (cur < end_excl) {
        vma_region_t* region = vma_find(mem, cur);

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

    if (size == 0u) {
        return 0;
    }

    uint32_t aligned_size = align_up_4k(size);
    uint32_t vaddr = align_up_4k(mem->mmap_top);

    if (mem->heap_start < mem->prog_break) {
        uint64_t need64 = (uint64_t)mem->prog_break + 0x100000ull;

        if (need64 < user_addr_max) {
            uint32_t need = align_up_4k((uint32_t)need64);

            if (vaddr < need) {
                vaddr = need;
            }
        }
    }

    for (int iter = 0; iter < 256; iter++) {
        uint32_t end_excl = vaddr + aligned_size;

        if (end_excl < vaddr || end_excl > user_addr_max) {
            return 0;
        }

        vma_region_t* overlap = vma_find_overlap(mem, vaddr, end_excl);

        if (!overlap) {
            *out_vaddr = vaddr;
            return 1;
        }

        vaddr = align_up_4k(overlap->vaddr_end);
    }

    return 0;
}

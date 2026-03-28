/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <arch/i386/paging.h>

#include <lib/string.h>

#include "api.h"

#ifndef PAGE_SIZE

#define PAGE_SIZE 4096u

#endif

static uint32_t size_to_order(size_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (pages == 0u) {
        return 0u;
    }

    uint32_t order = 0u;
    uint32_t pow2 = 1u;
    
    while (pow2 < pages) {
        pow2 <<= 1u;
        order++;
    }
    
    return order;
}

__attribute__((always_inline)) static inline void* __dma_alloc(size_t size, uint32_t* out_phys, uint32_t pte_flags) {
    if (size == 0u || !out_phys) {
        return 0;
    }

    uint32_t order = size_to_order(size);
    uint32_t pages = 1u << order;

    void* phys_ptr = pmm_alloc_pages(order);
    if (!phys_ptr) {
        return 0;
    }
    uint32_t phys_addr = (uint32_t)(uintptr_t)phys_ptr;

    void* virt_ptr = vmm_reserve_pages(pages);
    if (!virt_ptr) {
        pmm_free_pages(phys_ptr, order);
        return 0;
    }
    uint32_t virt_addr = (uint32_t)(uintptr_t)virt_ptr;
    
    for (uint32_t i = 0u; i < pages; i++) {
        paging_map(kernel_page_directory, 
                   virt_addr + i * PAGE_SIZE, 
                   phys_addr + i * PAGE_SIZE, 
                   pte_flags);
    }

    memset(virt_ptr, 0, size);
    
    *out_phys = phys_addr;
    return virt_ptr;
}

void* dma_alloc_coherent(size_t size, uint32_t* out_phys) {
    return __dma_alloc(size, out_phys, PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT);
}

void* dma_alloc_coherent_wc(size_t size, uint32_t* out_phys) {
    uint32_t flags = PTE_PRESENT | PTE_RW;
    
    if (paging_pat_is_supported()) {
        flags |= PTE_PAT;
    } else {
        flags |= PTE_PCD | PTE_PWT; 
    }
    
    return __dma_alloc(size, out_phys, flags);
}

void dma_free_coherent(void* vaddr, size_t size, uint32_t phys) {
    if (!vaddr) {
        return;
    }
    
    uint32_t order = size_to_order(size);
    uint32_t pages = 1u << order;
    uint32_t virt_start = (uint32_t)(uintptr_t)vaddr;

    paging_unmap_range(kernel_page_directory, virt_start, virt_start + pages * PAGE_SIZE);

    pmm_free_pages((void*)(uintptr_t)phys, order);

    vmm_unreserve_pages(vaddr, pages);
}

dma_sg_list_t* dma_map_buffer(void* vaddr, size_t size, uint32_t direction) {
    if (!vaddr
        || size == 0u) {
        return 0;
    }

    dma_sg_list_t* sg = (dma_sg_list_t*)kmalloc(sizeof(dma_sg_list_t));

    if (!sg) {
        return 0;
    }

    memset(sg, 0, sizeof(*sg));

    const uint32_t max_elems = (uint32_t)(size / PAGE_SIZE) + 2u;

    dma_sg_elem_t* elems = (dma_sg_elem_t*)kmalloc(max_elems * sizeof(dma_sg_elem_t));

    if (!elems) {
        kfree(sg);

        return 0;
    }

    memset(elems, 0, max_elems * sizeof(*elems));

    uint32_t remaining = (uint32_t)size;
    uint32_t current_vaddr = (uint32_t)(uintptr_t)vaddr;

    uint32_t elem_count = 0u;

    uint64_t last_phys = 0u;
    uint32_t last_len = 0u;

    while (remaining > 0u) {
        const uint32_t phys = paging_get_phys(kernel_page_directory, current_vaddr);

        if (phys == 0u) {
            kfree(elems);
            kfree(sg);

            return 0;
        }

        const uint32_t page_offset = current_vaddr & 0xFFFu;
        uint32_t chunk = 4096u - page_offset;

        if (chunk > remaining) {
            chunk = remaining;
        }

        if (elem_count > 0u
            && last_phys + last_len == phys) {

            last_len += chunk;
            elems[elem_count - 1u].length = last_len;

        } else {
            elems[elem_count].phys_addr = phys;
            elems[elem_count].length = chunk;

            elem_count++;

            last_phys = phys;
            last_len = chunk;
        }

        current_vaddr += chunk;
        remaining -= chunk;
    }

    sg->elems = elems;
    sg->count = elem_count;
    sg->capacity = max_elems;
    sg->direction = direction;

    return sg;
}

void dma_unmap_buffer(dma_sg_list_t* sg) {
    if (!sg) {
        return;
    }

    if (sg->elems) {
        kfree(sg->elems);
        sg->elems = 0;
    }

    kfree(sg);
}

uint32_t dma_virt_to_phys(void* vaddr) {
    if (!vaddr) {
        return 0u;
    }
    
    return paging_get_phys(kernel_page_directory, (uint32_t)(uintptr_t)vaddr);
}
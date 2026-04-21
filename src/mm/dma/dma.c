/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <arch/i386/paging.h>

#include "api.h"

#ifndef PAGE_SIZE

#define PAGE_SIZE 4096u

#endif

___inline uint32_t size_to_order(size_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    
    if (unlikely(pages <= 1u)) {
        return 0u;
    }

    return 32u - __builtin_clz(pages - 1u);
}

___inline void* __dma_alloc(size_t size, uint32_t* out_phys, uint32_t pte_flags) {
    if (unlikely(size == 0u || !out_phys)) {
        return 0;
    }

    const uint32_t order = size_to_order(size);
    const uint32_t pages = 1u << order;

    void* phys_ptr = pmm_alloc_pages(order);

    if (unlikely(!phys_ptr)) {
        return 0;
    }

    const uint32_t phys_addr = (uint32_t)(uintptr_t)phys_ptr;

    void* virt_ptr = vmm_reserve_pages(pages);

    if (unlikely(!virt_ptr)) {
        pmm_free_pages(phys_ptr, order);
        return 0;
    }

    const uint32_t virt_addr = (uint32_t)(uintptr_t)virt_ptr;
    
    uint32_t curr_v = virt_addr;
    uint32_t curr_p = phys_addr;

    for (uint32_t i = 0u; i < pages; i++) {
        paging_map_ex(kernel_page_directory, curr_v, curr_p, pte_flags, PAGING_MAP_NO_TLB_FLUSH);
        curr_v += PAGE_SIZE;
        curr_p += PAGE_SIZE;
    }

    /*
     * Don't do a TLB shootdown here, because according to the specification,
     * the lack of mapping is not cached by the processor. It's safe not to
     * invalidate TLB on other cores by sending IPI interrupts, since the
     * mapping is new. dma_free_coherent() uses paging_unmap_range(), which
     * automatically invalidates TLB on the required cores during unmapping.
     */

    uint8_t* zero_ptr = (uint8_t*)virt_ptr;
    for (uint32_t i = 0u; i < pages; i++) {
        memzero_nt_page(zero_ptr);

        zero_ptr += PAGE_SIZE;
    }
    
    *out_phys = phys_addr;
    return virt_ptr;
}

void* dma_alloc_coherent(size_t size, uint32_t* out_phys) {
    return __dma_alloc(size, out_phys, PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT);
}

void* dma_alloc_coherent_wc(size_t size, uint32_t* out_phys) {
    uint32_t flags = PTE_PRESENT | PTE_RW;
    
    if (likely(paging_pat_is_supported())) {
        flags |= PTE_PAT;
    } else {
        flags |= PTE_PCD | PTE_PWT; 
    }
    
    return __dma_alloc(size, out_phys, flags);
}

void dma_free_coherent(void* vaddr, size_t size, uint32_t phys) {
    if (unlikely(!vaddr)) {
        return;
    }
    
    const uint32_t order = size_to_order(size);
    const uint32_t pages = 1u << order;

    const uint32_t virt_start = (uint32_t)(uintptr_t)vaddr;

    paging_unmap_range(kernel_page_directory, virt_start, virt_start + pages * PAGE_SIZE);

    pmm_free_pages((void*)(uintptr_t)phys, order);
    
    vmm_unreserve_pages(vaddr, pages);
}

dma_sg_list_t* dma_map_buffer(void* vaddr, size_t size, uint32_t direction) {
    if (unlikely(!vaddr || size == 0u)) {
        return 0;
    }

    const uint32_t max_elems = (uint32_t)((((uintptr_t)vaddr & 0xFFFu) + size + 0xFFFu) >> 12u);

    const uint32_t alloc_size = sizeof(dma_sg_list_t) + (max_elems * sizeof(dma_sg_elem_t));

    dma_sg_list_t* sg = (dma_sg_list_t*)kmalloc(alloc_size);

    if (unlikely(!sg)) {
        return 0;
    }

    dma_sg_elem_t* elems = (dma_sg_elem_t*)(sg + 1);

    uint32_t remaining = (uint32_t)size;

    uint32_t current_vaddr = (uint32_t)(uintptr_t)vaddr;
    
    uint32_t elem_count = 0u;

    uint64_t last_phys = 0u;
    uint32_t last_len = 0u;

    while (remaining > 0u) {
        const uint32_t phys = paging_get_phys(kernel_page_directory, current_vaddr);

        if (unlikely(phys == 0u)) {
            kfree(sg);
            return 0;
        }

        const uint32_t page_offset = current_vaddr & 0xFFFu;
        const uint32_t chunk = PAGE_SIZE - page_offset;
        
        const uint32_t to_add = likely(chunk > remaining) ? remaining : chunk;

        if (likely(elem_count > 0u) && likely(last_phys + last_len == phys)) {
            last_len += to_add;
            
            elems[elem_count - 1u].length = last_len;
        } else {
            elems[elem_count].phys_addr = phys;
            elems[elem_count].length = to_add;

            elem_count++;

            last_phys = phys;
            last_len = to_add;
        }

        current_vaddr += to_add;
        remaining -= to_add;
    }

    sg->elems = elems;
    sg->count = elem_count;
    sg->capacity = max_elems;
    sg->direction = direction;

    return sg;
}

void dma_unmap_buffer(dma_sg_list_t* sg) {
    if (unlikely(!sg)) {
        return;
    }

    kfree(sg);
}

uint32_t dma_virt_to_phys(void* vaddr) {
    if (unlikely(!vaddr)) {
        return 0u;
    }
    
    return paging_get_phys(kernel_page_directory, (uint32_t)(uintptr_t)vaddr);
}
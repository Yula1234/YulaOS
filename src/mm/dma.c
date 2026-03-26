/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <mm/dma.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <arch/i386/paging.h>

#include <lib/string.h>

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

void* dma_alloc_coherent(size_t size, uint32_t* out_phys) {
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

    uint32_t pte_flags = PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT;
    
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

uint32_t dma_virt_to_phys(void* vaddr) {
    if (!vaddr) {
        return 0u;
    }
    
    return paging_get_phys(kernel_page_directory, (uint32_t)(uintptr_t)vaddr);
}
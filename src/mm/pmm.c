// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <hal/lock.h>

#include "pmm.h"

typedef struct {
    page_t* head;
    uint32_t count;
} free_area_t;

static page_t* mem_map = 0;
static uint32_t total_pages = 0;
static uint32_t used_pages_count = 0;

static free_area_t free_areas[PMM_MAX_ORDER + 1];

static spinlock_t pmm_lock;

static inline uint32_t align_up(uint32_t addr) {
    return PAGE_ALIGN(addr);
}

static void list_add(page_t** head, page_t* page) {
    page->next = *head;
    page->prev = 0;
    if (*head) {
        (*head)->prev = page;
    }
    *head = page;
}

static void list_remove(page_t** head, page_t* page) {
    if (page->prev) {
        page->prev->next = page->next;
    } else {
        *head = page->next;
    }
    if (page->next) {
        page->next->prev = page->prev;
    }
    page->next = 0;
    page->prev = 0;
}

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr) {
    spinlock_init(&pmm_lock);

    total_pages = mem_size / PAGE_SIZE;
    
    for (int i = 0; i <= PMM_MAX_ORDER; i++) {
        free_areas[i].head = 0;
        free_areas[i].count = 0;
    }

    uint32_t mem_map_phys = align_up(kernel_end_addr);
    mem_map = (page_t*)mem_map_phys;

    uint32_t mem_map_size = total_pages * sizeof(page_t);
    memset(mem_map, 0, mem_map_size);

    uint32_t phys_alloc_start = align_up(mem_map_phys + mem_map_size);
    uint32_t first_free_idx = phys_alloc_start / PAGE_SIZE;

    used_pages_count = total_pages;

    for (uint32_t i = 0; i < first_free_idx; i++) {
        mem_map[i].flags = PMM_FLAG_USED | PMM_FLAG_KERNEL;
        mem_map[i].ref_count = 1;
        mem_map[i].order = 0;
    }

    uint32_t i = first_free_idx;
    uint32_t max_block_size = (1 << PMM_MAX_ORDER);

    while (i < total_pages && (i & (max_block_size - 1)) != 0) {
        mem_map[i].flags = PMM_FLAG_USED; 
        pmm_free_pages((void*)(i * PAGE_SIZE), 0);
        i++;
    }

    while (i + max_block_size <= total_pages) {
        page_t* page = &mem_map[i];
        
        page->flags = PMM_FLAG_FREE;
        page->order = PMM_MAX_ORDER;
        page->ref_count = 0;
        
        list_add(&free_areas[PMM_MAX_ORDER].head, page);
        free_areas[PMM_MAX_ORDER].count++;
        
        used_pages_count -= max_block_size;
        
        i += max_block_size;
    }

    while (i < total_pages) {
        mem_map[i].flags = PMM_FLAG_USED;
        pmm_free_pages((void*)(i * PAGE_SIZE), 0);
        i++;
    }
}

void* pmm_alloc_pages(uint32_t order) {
    if (order > PMM_MAX_ORDER) return 0;
    uint32_t flags = spinlock_acquire_safe(&pmm_lock);

    uint32_t current_order = order;
    while (current_order <= PMM_MAX_ORDER) {
        if (free_areas[current_order].head) {
            break;
        }
        current_order++;
    }

    if (current_order > PMM_MAX_ORDER) {
        spinlock_release_safe(&pmm_lock, flags);
        return 0;
    }

    page_t* page = free_areas[current_order].head;
    list_remove(&free_areas[current_order].head, page);
    free_areas[current_order].count--;
    
    page->flags |= PMM_FLAG_USED;
    page->flags &= ~PMM_FLAG_FREE;
    page->ref_count = 1;

    while (current_order > order) {
        current_order--;
        
        uint32_t pfn = page - mem_map;
        
        uint32_t buddy_pfn = pfn + (1 << current_order);
        page_t* buddy = &mem_map[buddy_pfn];

        buddy->flags = PMM_FLAG_FREE;
        buddy->order = current_order;
        buddy->ref_count = 0;
        
        list_add(&free_areas[current_order].head, buddy);
        free_areas[current_order].count++;
    }

    page->order = order;
    used_pages_count += (1 << order);

    spinlock_release_safe(&pmm_lock, flags);
    return (void*)pmm_page_to_phys(page);
}

void pmm_free_pages(void* addr, uint32_t order) {
    if (!addr) return;
    if (order > PMM_MAX_ORDER) return;

    page_t* page = pmm_phys_to_page((uint32_t)addr);
    if (!page) return;
    uint32_t flags = spinlock_acquire_safe(&pmm_lock);

    if (page->flags == PMM_FLAG_FREE) {
        spinlock_release_safe(&pmm_lock, flags);
        return;
    }

    used_pages_count -= (1 << order);
    
    uint32_t pfn = page - mem_map;

    while (order < PMM_MAX_ORDER) {
        uint32_t buddy_pfn = pfn ^ (1 << order);
        page_t* buddy = &mem_map[buddy_pfn];

        if (buddy_pfn >= total_pages) break;
        
        if (!(buddy->flags == PMM_FLAG_FREE)) break;
        
        if (buddy->order != order) break;

        list_remove(&free_areas[order].head, buddy);
        free_areas[order].count--;

        buddy->order = 0;

        pfn &= buddy_pfn; 
        page = &mem_map[pfn];

        order++;
    }

    page->flags = PMM_FLAG_FREE;
    page->order = order;
    page->ref_count = 0;
    
    list_add(&free_areas[order].head, page);
    free_areas[order].count++;

    spinlock_release_safe(&pmm_lock, flags);
}


void* pmm_alloc_block() {
    return pmm_alloc_pages(0);
}

void pmm_free_block(void* addr) {
    pmm_free_pages(addr, 0);
}

page_t* pmm_phys_to_page(uint32_t phys_addr) {
    uint32_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages) return 0;
    return &mem_map[idx];
}

uint32_t pmm_page_to_phys(page_t* page) {
    uint32_t idx = page - mem_map;
    return idx * PAGE_SIZE;
}

uint32_t pmm_get_used_blocks() { return used_pages_count; }
uint32_t pmm_get_free_blocks() { return total_pages - used_pages_count; }
uint32_t pmm_get_total_blocks() { return total_pages; }
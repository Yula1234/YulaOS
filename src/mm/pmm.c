#include <lib/string.h>
#include <hal/lock.h>

#include "pmm.h"

static page_t* mem_map = 0;
static uint32_t total_pages = 0;

static page_t* free_list_head = 0;

static spinlock_t pmm_lock;

static uint32_t used_pages_count = 0;


static inline uint32_t align_up(uint32_t addr) {
    return PAGE_ALIGN(addr);
}

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr) {
    spinlock_init(&pmm_lock);

    total_pages = mem_size / PAGE_SIZE;
    
    uint32_t mem_map_phys = align_up(kernel_end_addr);
    mem_map = (page_t*)mem_map_phys;

    uint32_t mem_map_size = total_pages * sizeof(page_t);

    memset(mem_map, 0, mem_map_size);

    uint32_t phys_alloc_start = align_up(mem_map_phys + mem_map_size);
    
    uint32_t first_free_idx = phys_alloc_start / PAGE_SIZE;

    for (uint32_t i = 0; i < total_pages; i++) {
        mem_map[i].flags = PMM_FLAG_USED | PMM_FLAG_KERNEL;
        mem_map[i].ref_count = 1;
        mem_map[i].next = 0;
    }

    for (uint32_t i = total_pages - 1; i >= first_free_idx; i--) {
        page_t* page = &mem_map[i];
        
        page->flags = PMM_FLAG_FREE;
        page->ref_count = 0;
        
        page->next = free_list_head;
        free_list_head = page;
    }

    used_pages_count = first_free_idx;
}

void* pmm_alloc_block() {
    uint32_t flags = spinlock_acquire_safe(&pmm_lock);

    if (!free_list_head) {
        spinlock_release_safe(&pmm_lock, flags);
        return 0; 
    }

    page_t* page = free_list_head;
    free_list_head = page->next;

    page->flags |= PMM_FLAG_USED;
    page->flags &= ~PMM_FLAG_FREE;
    page->ref_count = 1;
    page->next = 0;

    used_pages_count++;

    spinlock_release_safe(&pmm_lock, flags);

    return (void*)pmm_page_to_phys(page);
}

void pmm_free_block(void* addr) {
    if (!addr) return;

    page_t* page = pmm_phys_to_page((uint32_t)addr);
    
    if (!page) return; 

    uint32_t flags = spinlock_acquire_safe(&pmm_lock);

    if (page->ref_count > 0) {
        page->ref_count--;
    }

    if (page->ref_count == 0) {
        page->flags = PMM_FLAG_FREE;
        
        page->slab_cache = 0;
        page->freelist = 0;
        page->objects = 0;

        page->next = free_list_head;
        free_list_head = page;

        used_pages_count--;
    }

    spinlock_release_safe(&pmm_lock, flags);
}

page_t* pmm_phys_to_page(uint32_t phys_addr) {
    uint32_t idx = phys_addr / PAGE_SIZE;
    if (idx >= total_pages) return 0;
    return &mem_map[idx];
}

uint32_t pmm_page_to_phys(page_t* page) {
    uint32_t idx = page - mem_map;
    if (idx >= total_pages) return 0;
    return idx * PAGE_SIZE;
}

uint32_t pmm_get_used_blocks() { return used_pages_count; }
uint32_t pmm_get_free_blocks() { return total_pages - used_pages_count; }
uint32_t pmm_get_total_blocks() { return total_pages; }
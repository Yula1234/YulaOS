#include <lib/string.h>
#include <hal/lock.h>
#include <arch/i386/paging.h>
#include <mm/pmm.h>
#include <drivers/vga.h>

#include "vmm.h"

#define BITMAP_SIZE (KERNEL_HEAP_SIZE / PAGE_SIZE / 8)

static uint8_t vmm_bitmap[BITMAP_SIZE];
static spinlock_t vmm_lock;
static size_t vmm_used_pages = 0;

extern uint32_t* kernel_page_directory;

static void set_bit(uint32_t bit) {
    vmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

static void clear_bit(uint32_t bit) {
    vmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int test_bit(uint32_t bit) {
    return vmm_bitmap[bit / 8] & (1 << (bit % 8));
}

void vmm_init(void) {
    spinlock_init(&vmm_lock);
    memset(vmm_bitmap, 0, BITMAP_SIZE);
    vmm_used_pages = 0;
}

static int vmm_find_free_range(size_t count) {
    size_t total_pages = KERNEL_HEAP_SIZE / PAGE_SIZE;
    size_t found = 0;
    int start_bit = -1;

    for (size_t i = 0; i < total_pages; i++) {
        if (!test_bit(i)) {
            if (found == 0) start_bit = i;
            found++;
            if (found == count) return start_bit;
        } else {
            found = 0;
            start_bit = -1;
        }
    }
    return -1;
}

void* vmm_alloc_pages(size_t pages) {
    if (pages == 0) return 0;

    uint32_t flags = spinlock_acquire_safe(&vmm_lock);

    int start_bit = vmm_find_free_range(pages);
    if (start_bit == -1) {
        spinlock_release_safe(&vmm_lock, flags);
        return 0;
    }

    for (size_t i = 0; i < pages; i++) {
        set_bit(start_bit + i);
    }
    
    vmm_used_pages += pages;

    uint32_t virt_start = KERNEL_HEAP_START + (start_bit * PAGE_SIZE);

    for (size_t i = 0; i < pages; i++) {
        void* phys = pmm_alloc_block();
        if (!phys) {
            spinlock_release_safe(&vmm_lock, flags);
            return 0;
        }
        
        uint32_t vaddr = virt_start + i * PAGE_SIZE;
        paging_map(kernel_page_directory, vaddr, (uint32_t)phys, 3);
    }

    spinlock_release_safe(&vmm_lock, flags);
    return (void*)virt_start;
}

void vmm_free_pages(void* virt, size_t pages) {
    if (!virt || pages == 0) return;

    uint32_t vaddr = (uint32_t)virt;
    if (vaddr < KERNEL_HEAP_START || vaddr >= KERNEL_HEAP_START + KERNEL_HEAP_SIZE) return;

    uint32_t start_bit = (vaddr - KERNEL_HEAP_START) / PAGE_SIZE;
    
    uint32_t flags = spinlock_acquire_safe(&vmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint32_t curr_v = vaddr + i * PAGE_SIZE;
        
        uint32_t phys = paging_get_phys(kernel_page_directory, curr_v);
        if (phys) {
            pmm_free_block((void*)phys);
        }

        __asm__ volatile("invlpg (%0)" :: "r" (curr_v) : "memory");
        clear_bit(start_bit + i);
    }
    
    if (vmm_used_pages >= pages) vmm_used_pages -= pages;
    else vmm_used_pages = 0;

    spinlock_release_safe(&vmm_lock, flags);
}

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    paging_map(kernel_page_directory, virt, phys, flags);
    return 0;
}

size_t vmm_get_used_pages(void) {
    return vmm_used_pages;
}
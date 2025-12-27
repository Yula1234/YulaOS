#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <hal/lock.h>
#include "paging.h"

extern void load_page_directory(uint32_t*);
extern void enable_paging(void);

uint32_t* kernel_page_directory = 0;

uint32_t page_dir[1024] __attribute__((aligned(4096)));

static spinlock_t paging_lock;

static inline uint32_t* read_cr3(void) {
    uint32_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return (uint32_t*)val;
}

void paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_lock);

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & 1)) {
        void* new_pt_phys = pmm_alloc_block();
        
        uint32_t* new_pt_virt = (uint32_t*)new_pt_phys;
        
        memset(new_pt_virt, 0, 4096);

        dir[pd_idx] = ((uint32_t)new_pt_phys) | 7;
    }

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
    pt[pt_idx] = (phys & ~0xFFF) | flags;

    __asm__ volatile("invlpg (%0)" :: "r" (virt) : "memory");

    spinlock_release_safe(&paging_lock, int_flags);
}

static void paging_allocate_table(uint32_t virt) {
    uint32_t int_flags = spinlock_acquire_safe(&paging_lock);
    
    uint32_t pd_idx = virt >> 22;
    if (!(kernel_page_directory[pd_idx] & 1)) {
        void* new_pt_phys = pmm_alloc_block();
        if (new_pt_phys) {
            memset(new_pt_phys, 0, 4096);
            kernel_page_directory[pd_idx] = ((uint32_t)new_pt_phys) | 7;
        }
    }
    
    spinlock_release_safe(&paging_lock, int_flags);
}

void paging_init(uint32_t ram_size_bytes) {
    spinlock_init(&paging_lock);

    for(int i = 0; i < 1024; i++) {
        page_dir[i] = 2; 
    }

    if (ram_size_bytes & 0xFFF) ram_size_bytes = (ram_size_bytes & ~0xFFF) + 4096;

    for(uint32_t i = 0; i < ram_size_bytes; i += 4096) { 
        uint32_t pd_idx = i >> 22;
        uint32_t pt_idx = (i >> 12) & 0x3FF;

        if (!(page_dir[pd_idx] & 1)) {
            void* pt_phys = pmm_alloc_block();
            memset(pt_phys, 0, 4096);
            page_dir[pd_idx] = ((uint32_t)pt_phys) | 3; // Supervisor | RW | Present
        }
        
        uint32_t* pt = (uint32_t*)(page_dir[pd_idx] & ~0xFFF);
        pt[pt_idx] = i | 3; // Supervisor | RW | Present
        
        if (i + 4096 < i) break;
    }

    paging_map(page_dir, 0xFEE00000, 0xFEE00000, 3);

    kernel_page_directory = page_dir;

    for (uint32_t addr = 0xD0000000; addr < 0xE0000000; addr += 0x400000) {
        paging_allocate_table(addr);
    }

    paging_switch(page_dir);
    enable_paging();
}

void paging_switch(uint32_t* dir_phys) {
    load_page_directory(dir_phys);
}

uint32_t* paging_get_dir(void) {
    return read_cr3();
}

uint32_t* paging_clone_directory(void) {
    uint32_t* new_dir = (uint32_t*)pmm_alloc_block();
    if (!new_dir) return 0;

    memset(new_dir, 0, 4096);

    for (int i = 0; i < 1024; i++) {
        if (kernel_page_directory[i] & 1) {
            new_dir[i] = kernel_page_directory[i];
        }
    }
    return new_dir;
}

int paging_is_user_accessible(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & 1)) return 0;
    if (!(dir[pd_idx] & 4)) return 0;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
    if (!(pt[pt_idx] & 1)) return 0;
    if (!(pt[pt_idx] & 4)) return 0;

    return 1;
}

uint32_t paging_get_phys(uint32_t* dir, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(dir[pd_idx] & 1)) return 0;

    uint32_t* pt = (uint32_t*)(dir[pd_idx] & ~0xFFF);
    if (!(pt[pt_idx] & 1)) return 0;

    return (pt[pt_idx] & ~0xFFF) + (virt & 0xFFF);
}
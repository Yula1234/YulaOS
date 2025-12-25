#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_ALIGN(x)   (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))

typedef enum {
    PMM_FLAG_FREE     = 0,
    PMM_FLAG_USED     = (1 << 0),
    PMM_FLAG_KERNEL   = (1 << 1),
    PMM_FLAG_DMA      = (1 << 2),
    PMM_FLAG_SLAB     = (1 << 3),
} page_flags_t;

typedef struct page {
    uint32_t flags; 
    int32_t  ref_count;

    void* slab_cache;  
    void* freelist;    
    uint16_t objects;  

    struct page* next;
} page_t;

void pmm_init(uint32_t mem_size, uint32_t kernel_end_addr);

void* pmm_alloc_block(void);     
void  pmm_free_block(void* addr);

page_t*  pmm_phys_to_page(uint32_t phys_addr);
uint32_t pmm_page_to_phys(page_t* page);

uint32_t pmm_get_used_blocks(void);
uint32_t pmm_get_free_blocks(void);
uint32_t pmm_get_total_blocks(void);

#endif
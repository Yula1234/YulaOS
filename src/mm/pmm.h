#ifndef MM_PMM_H
#define MM_PMM_H

#include <stdint.h>

void  pmm_init(uint32_t mem_size);
void* pmm_alloc_block(void);
void  pmm_free_block(void* addr);

uint32_t pmm_get_used_blocks();
uint32_t pmm_get_free_blocks();

#endif
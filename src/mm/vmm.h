#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>
#include <stddef.h>

#define KERNEL_HEAP_START 0xD0000000
#define KERNEL_HEAP_SIZE  (256 * 1024 * 1024)
#define PAGE_SIZE         4096

void vmm_init(void);
void* vmm_alloc_pages(size_t pages);
void  vmm_free_pages(void* virt, size_t pages);
int   vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
size_t vmm_get_used_pages(void);

#endif
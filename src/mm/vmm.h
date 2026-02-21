// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef MM_VMM_H
#define MM_VMM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KERNEL_HEAP_START 0xC0000000u
#define KERNEL_HEAP_SIZE  0x40000000u
#define PAGE_SIZE         4096

void vmm_init(void);

void* vmm_alloc_pages(size_t pages);
void vmm_free_pages(void* virt, size_t pages);

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

size_t vmm_get_used_pages(void);

#ifdef __cplusplus
}
#endif

#endif
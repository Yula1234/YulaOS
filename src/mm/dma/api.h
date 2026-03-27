/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef HAL_DMA_H
#define HAL_DMA_H

#include <stdint.h>
#include <stddef.h>

#define DMA_DIR_TO_DEVICE 1u
#define DMA_DIR_FROM_DEVICE 2u
#define DMA_DIR_BIDIRECTIONAL 3u

typedef struct dma_sg_elem {
    uint64_t phys_addr;
    uint32_t length;
} dma_sg_elem_t;

typedef struct dma_sg_list {
    dma_sg_elem_t* elems;

    uint32_t count;
    uint32_t capacity;
    uint32_t direction;
} dma_sg_list_t;

typedef struct dma_pool dma_pool_t;

#ifdef __cplusplus
extern "C" {
#endif

void* dma_alloc_coherent(size_t size, uint32_t* out_phys);
void dma_free_coherent(void* vaddr, size_t size, uint32_t phys);

dma_sg_list_t* dma_map_buffer(void* vaddr, size_t size, uint32_t direction);
void dma_unmap_buffer(dma_sg_list_t* sg);

uint32_t dma_virt_to_phys(void* vaddr);

dma_pool_t* dma_pool_create(const char* name, size_t obj_size, size_t align);

void* dma_pool_alloc(dma_pool_t* pool, uint32_t* out_phys);

void dma_pool_free(dma_pool_t* pool, void* vaddr);

void dma_pool_destroy(dma_pool_t* pool);

#ifdef __cplusplus
}
#endif

#endif
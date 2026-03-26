/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef HAL_DMA_H
#define HAL_DMA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* dma_alloc_coherent(size_t size, uint32_t* out_phys);
void dma_free_coherent(void* vaddr, size_t size, uint32_t phys);

uint32_t dma_virt_to_phys(void* vaddr);

#ifdef __cplusplus
}
#endif

#endif
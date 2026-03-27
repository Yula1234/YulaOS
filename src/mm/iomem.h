// SPDX-License-Identifier: GPL-2.0

#ifndef MM_IOMEM_H
#define MM_IOMEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct __iomem_region;
typedef struct __iomem_region __iomem;

__iomem* iomem_request_mmio(uint32_t phys_start, uint32_t size, const char* name);
__iomem* iomem_request_mmio_wc(uint32_t phys_start, uint32_t size, const char* name);
__iomem* iomem_request_pmio(uint16_t port_start, uint16_t size, const char* name);

void iomem_free(__iomem* region);

uint8_t ioread8(__iomem* region, uint32_t offset);
uint16_t ioread16(__iomem* region, uint32_t offset);
uint32_t ioread32(__iomem* region, uint32_t offset);

void iowrite8(__iomem* region, uint32_t offset, uint8_t value);
void iowrite16(__iomem* region, uint32_t offset, uint16_t value);
void iowrite32(__iomem* region, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
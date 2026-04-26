/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef HAL_MMIO_H
#define HAL_MMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mmio_region;
typedef struct mmio_region mmio_region_t;

/*
 * mmio_init() must be called during early boot (e.g., alongside pmio_init).
 */
void mmio_init(void);

/*
 * Request ownership of a physical memory region and map it into virtual space.
 * By default, memory is mapped as Uncacheable (UC), which is required for
 * device control registers.
 *
 * Returns a region handle on success, or 0 on failure (overlap, OOM, or invalid size).
 */
mmio_region_t* mmio_request_region(uint32_t phys_start, uint32_t size, const char* name);

/*
 * Request a region with Write-Combining (WC) caching.
 * This is highly recommended for framebuffers to vastly improve write performance,
 * but MUST NOT be used for device control registers.
 */
mmio_region_t* mmio_request_region_wc(uint32_t phys_start, uint32_t size, const char* name);

/*
 * Unmap the virtual memory and release the physical region claim.
 */
void mmio_release_region(mmio_region_t* region);

/*
 * Get the mapped virtual address of the region.
 * This is necessary for drivers that need direct pointer access 
 * (e.g., Framebuffers or DMA descriptors).
 */
void* mmio_get_vaddr(mmio_region_t* region);

/*
 * Safe capability-style accessors.
 * Offsets are strictly validated against region bounds.
 * Accesses are performed using volatile to ensure the compiler does not optimize them out.
 */
int mmio_read8(mmio_region_t* region, uint32_t offset, uint8_t* out_value);
int mmio_read16(mmio_region_t* region, uint32_t offset, uint16_t* out_value);
int mmio_read32(mmio_region_t* region, uint32_t offset, uint32_t* out_value);

int mmio_write8(mmio_region_t* region, uint32_t offset, uint8_t value);
int mmio_write16(mmio_region_t* region, uint32_t offset, uint16_t value);
int mmio_write32(mmio_region_t* region, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif // HAL_MMIO_H
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef HAL_PMIO_H
#define HAL_PMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PMIO (port-mapped I/O) region tracking.
 *
 * This is a lightweight ownership layer for x86 I/O ports. Drivers are
 * expected to reserve the port ranges they use so that overlapping claims
 * can be rejected early and reported through the caller-visible API.
 *
 * The region object returned from pmio_request_region() is an opaque handle
 * owned by the PMIO subsystem; it must be released with pmio_release_region().
 */

struct pmio_region;
typedef struct pmio_region pmio_region_t;

/*
 * pmio_init() must be called once during early boot, after heap_init(),
 * before drivers start requesting regions.
 */
void pmio_init(void);

/*
 * Request ownership of a port range [start, start + count - 1].
 *
 * Returns a region handle on success or NULL on invalid input, OOM,
 * or overlap with an existing region.
 */
pmio_region_t* pmio_request_region(uint16_t start, uint16_t count, const char* name);

/*
 * Drop ownership previously obtained via pmio_request_region().
 */
void pmio_release_region(pmio_region_t* region);

/*
 * Optional per-device serialization.
 *
 * Some devices require multi-port sequences to be atomic with respect to
 * other users (index/data pairs, mode/data programming, etc.). Drivers may
 * bracket such sequences with pmio_acquire_bus()/pmio_release_bus().
 */
void pmio_acquire_bus(pmio_region_t* region);
void pmio_release_bus(pmio_region_t* region);

/*
 * Lookup helpers.
 *
 * pmio_find_region() returns the owner of a single port.
 * pmio_find_conflict() returns any region overlapping the requested range.
 */
pmio_region_t* pmio_find_region(uint16_t port);
pmio_region_t* pmio_find_conflict(uint16_t start, uint16_t count);

/*
 * Iterate over all currently registered regions.
 *
 * The callback is invoked in ascending tree order. The iterator does not
 * transfer ownership of any internal objects; it only exposes a snapshot of
 * (start, end, name) under the PMIO read lock.
 */
typedef void (*pmio_iterate_cb_t)(uint16_t start, uint16_t end, const char* name, void* ctx);
void pmio_iterate(pmio_iterate_cb_t callback, void* ctx);

/*
 * Capability-style port accessors.
 *
 * Offsets are validated against the region bounds, so drivers cannot
 * accidentally step outside their granted range.
 */
int pmio_readb(pmio_region_t* region, uint16_t offset, uint8_t* value);
int pmio_readw(pmio_region_t* region, uint16_t offset, uint16_t* value);
int pmio_readl(pmio_region_t* region, uint16_t offset, uint32_t* value);

int pmio_writeb(pmio_region_t* region, uint16_t offset, uint8_t value);
int pmio_writew(pmio_region_t* region, uint16_t offset, uint16_t value);
int pmio_writel(pmio_region_t* region, uint16_t offset, uint32_t value);

/*
 * String I/O operations (rep ins/outs).
 *
 * These transfer count elements of the given width to/from a buffer.
 * The offset must be within region bounds, and the entire transfer
 * must fit within the region (offset + count * width <= region_size).
 *
 * Returns 0 on success, -1 on invalid parameters or bounds violation.
 */
int pmio_read_bufb(pmio_region_t* region, uint16_t offset, void* buf, uint32_t count);
int pmio_read_bufw(pmio_region_t* region, uint16_t offset, void* buf, uint32_t count);
int pmio_read_bufl(pmio_region_t* region, uint16_t offset, void* buf, uint32_t count);

int pmio_write_bufb(pmio_region_t* region, uint16_t offset, const void* buf, uint32_t count);
int pmio_write_bufw(pmio_region_t* region, uint16_t offset, const void* buf, uint32_t count);
int pmio_write_bufl(pmio_region_t* region, uint16_t offset, const void* buf, uint32_t count);


#ifdef __cplusplus
}
#endif

#endif // HAL_PMIO_H
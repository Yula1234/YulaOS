// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef HAL_PMIO_H
#define HAL_PMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pmio_region;
typedef struct pmio_region pmio_region_t;

void pmio_init(void);

pmio_region_t* pmio_request_region(uint16_t start, uint16_t count, const char* name);
void pmio_release_region(pmio_region_t* region);

pmio_region_t* pmio_find_region(uint16_t port);
pmio_region_t* pmio_find_conflict(uint16_t start, uint16_t count);

typedef void (*pmio_iterate_cb_t)(uint16_t start, uint16_t end, const char* name, void* ctx);
void pmio_iterate(pmio_iterate_cb_t callback, void* ctx);

uint8_t  pmio_readb(uint16_t port);
uint16_t pmio_readw(uint16_t port);
uint32_t pmio_readl(uint16_t port);

void pmio_writeb(uint16_t port, uint8_t val);
void pmio_writew(uint16_t port, uint16_t val);
void pmio_writel(uint16_t port, uint32_t val);


#ifdef __cplusplus
}
#endif

#endif // HAL_PMIO_H
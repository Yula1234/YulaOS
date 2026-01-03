// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_IOAPIC_H
#define HAL_IOAPIC_H

#include <stdint.h>

int ioapic_init(uint32_t phys_addr, uint32_t gsi_base);
int ioapic_is_initialized(void);
int ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id, int active_low, int level_trigger);

#endif

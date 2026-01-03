// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_ACPI_H
#define DRIVERS_ACPI_H

#include <stdint.h>

void acpi_init(void);

int acpi_get_ioapic(uint32_t* out_phys_addr, uint32_t* out_gsi_base);
int acpi_get_iso(uint8_t source_irq, uint32_t* out_gsi, int* out_active_low, int* out_level_trigger);

#endif
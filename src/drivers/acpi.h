/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef DRIVERS_ACPI_H
#define DRIVERS_ACPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void acpi_init(void);

int acpi_get_ioapic(
    uint32_t* out_phys_addr,
    uint32_t* out_gsi_base
);

int acpi_get_iso(
    uint8_t source_irq,
    uint32_t* out_gsi,
    int* out_active_low,
    int* out_level_trigger
);

int acpi_get_mcfg(
    uint64_t* out_base_addr,
    uint16_t* out_pci_seg_group,
    uint8_t* out_start_bus,
    uint8_t* out_end_bus
);

void acpi_reboot(void);

void acpi_poweroff(void);

#ifdef __cplusplus
}
#endif

#endif
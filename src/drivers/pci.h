// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>

uint32_t pci_find_ide_bar4();
uint32_t pci_get_bar5(uint8_t bus, uint8_t slot, uint8_t func);
uint32_t pci_find_ahci_device(uint8_t* bus, uint8_t* slot, uint8_t* func);
uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
int pci_msi_configure(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector, uint8_t dest_apic_id);

#endif
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <drivers/driver.h>

#include <mm/iomem.h>

#include <hal/irq.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
int pci_msi_configure(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector, uint8_t dest_apic_id);

#define PCI_MATCH_VENDOR_ID  (1u << 0)
#define PCI_MATCH_DEVICE_ID  (1u << 1)
#define PCI_MATCH_CLASS      (1u << 2)
#define PCI_MATCH_SUBCLASS   (1u << 3)
#define PCI_MATCH_PROG_IF    (1u << 4)
#define PCI_MATCH_DEVICE_ID_RANGE (1u << 5)

typedef struct pci_device_id {
    uint32_t match_flags;

    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t device_id_last;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
} pci_device_id_t;

#define PCI_BAR_TYPE_MMIO  0u
#define PCI_BAR_TYPE_IO    1u

typedef struct pci_bar {
    uint32_t base_addr;
    uint32_t size;

    uint32_t type;
    uint32_t is_64bit;
    uint32_t is_prefetchable;
} pci_bar_t;

struct pci_driver;

typedef struct pci_device {
    device_t dev;

    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;

    pci_bar_t bars[6];

    uint8_t irq_line;
    uint8_t irq_pin;

    struct pci_driver* attached_driver;
} pci_device_t;

typedef struct pci_driver {
    driver_desc_t base;

    const pci_device_id_t* id_table;

    int (*probe)(pci_device_t* dev);
    void (*remove)(pci_device_t* dev);
} pci_driver_t;


int pci_register_driver(pci_driver_t* driver);

int pci_request_irq(pci_device_t* dev, irq_handler_t handler, void* ctx);

uint32_t pci_dev_read32(const pci_device_t* dev, uint8_t offset);
void pci_dev_write32(pci_device_t* dev, uint8_t offset, uint32_t value);

uint16_t pci_dev_read16(const pci_device_t* dev, uint8_t offset);
void pci_dev_write16(pci_device_t* dev, uint8_t offset, uint16_t value);

uint8_t pci_dev_read8(const pci_device_t* dev, uint8_t offset);
void pci_dev_write8(pci_device_t* dev, uint8_t offset, uint8_t value);

void pci_dev_enable_busmaster(pci_device_t* dev);
int pci_dev_enable_msi(pci_device_t* dev, uint8_t vector, uint8_t dest_apic_id);
int pci_dev_enable_msix(pci_device_t* dev, uint16_t entry, uint8_t vector, uint8_t dest_apic_id);

__iomem* pci_request_bar(pci_device_t* dev, uint8_t bar_idx, const char* name);

#ifdef __cplusplus
}
#endif

#endif
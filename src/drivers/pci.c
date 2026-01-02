// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <hal/io.h>

#include "pci.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFC);
    return (uint8_t)((reg >> ((offset & 3) * 8)) & 0xFF);
}

static inline uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFC);
    return (uint16_t)((reg >> ((offset & 2) * 8)) & 0xFFFF);
}

static inline void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint8_t aligned = offset & 0xFC;
    uint32_t reg = pci_read(bus, slot, func, aligned);
    uint32_t shift = (offset & 2) * 8;
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    pci_write(bus, slot, func, aligned, reg);
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_get_vendor(uint8_t bus, uint8_t slot, uint8_t func) {
    return (uint16_t)(pci_read(bus, slot, func, 0) & 0xFFFF);
}

uint16_t pci_get_class_sub(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg = pci_read(bus, slot, func, 0x08);
    return (uint16_t)((reg >> 16) & 0xFFFF); 
}

uint32_t pci_get_bar4(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar4 = pci_read(bus, slot, func, 0x20);
    return bar4 & 0xFFFFFFFC; 
}

void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t command = pci_read(bus, slot, func, 0x04);
    if ((command & 0x5) != 0x5) {
        pci_write(bus, slot, func, 0x04, command | 0x5);
    }
}

uint32_t pci_find_ide_bar4() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_get_vendor(bus, slot, 0);
            if (vendor != 0xFFFF) {
                if (pci_get_class_sub(bus, slot, 0) == 0x0101) {
                    pci_enable_bus_master(bus, slot, 0);
                    return pci_get_bar4(bus, slot, 0);
                }
            }
        }
    }
    return 0;
}

int pci_msi_configure(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector, uint8_t dest_apic_id) {
    uint32_t cmdsts = pci_read(bus, slot, func, 0x04);
    uint16_t status = (uint16_t)((cmdsts >> 16) & 0xFFFF);
    if (!(status & 0x0010)) {
        return 0;
    }

    uint8_t cap = pci_read8(bus, slot, func, 0x34);
    for (int iter = 0; iter < 48 && cap != 0; iter++) {
        uint8_t cap_id = pci_read8(bus, slot, func, cap + 0);
        uint8_t cap_next = pci_read8(bus, slot, func, cap + 1);

        if (cap_id == 0x05) {
            uint16_t control = pci_read16(bus, slot, func, cap + 2);
            int is_64 = (control & (1 << 7)) != 0;

            uint32_t msg_addr = 0xFEE00000u | ((uint32_t)dest_apic_id << 12);
            pci_write(bus, slot, func, cap + 4, msg_addr);

            uint8_t data_off = (uint8_t)(cap + (is_64 ? 12 : 8));
            if (is_64) {
                pci_write(bus, slot, func, cap + 8, 0);
            }

            uint16_t msg_data = (uint16_t)vector;
            pci_write16(bus, slot, func, data_off, msg_data);

            control &= (uint16_t)~(0x7 << 4);
            control |= 1;
            pci_write16(bus, slot, func, cap + 2, control);

            uint16_t command = (uint16_t)(cmdsts & 0xFFFF);
            command |= (1 << 10);
            pci_write16(bus, slot, func, 0x04, command);
            return 1;
        }

        cap = cap_next;
    }
    return 0;
}

uint32_t pci_get_bar5(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar5 = pci_read(bus, slot, func, 0x24);
    return bar5 & 0xFFFFFFF0;
}

uint32_t pci_find_ahci_device(uint8_t* out_bus, uint8_t* out_slot, uint8_t* out_func) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_get_vendor(bus, slot, func);
                if (vendor == 0xFFFF) continue;

                uint16_t class_sub = pci_get_class_sub(bus, slot, func);
                if (class_sub == 0x0106) {
                    *out_bus = (uint8_t)bus;
                    *out_slot = (uint8_t)slot;
                    *out_func = (uint8_t)func;
                    return 1;
                }
            }
        }
    }
    return 0;
}
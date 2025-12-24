#include "pci.h"
#include "../hal/io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

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
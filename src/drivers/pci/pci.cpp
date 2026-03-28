// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <drivers/pci/pci.h>

#include <hal/io.h>

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/new.h>
#include <lib/string.h>

#include <kernel/output/kprintf.h>
#include <kernel/smp/cpu.h>
#include <mm/heap.h>

extern "C" {
#include <hal/ioapic.h>
#include <hal/pic.h>
#include <drivers/acpi.h>
}

namespace kernel::pci {

constexpr uint32_t kPciConfigAddr = 0xCF8u;
constexpr uint32_t kPciConfigData = 0xCFCu;

constexpr uint8_t kPciCapIdMsi = 0x05u;
constexpr uint8_t kPciCapIdMsix = 0x11u;

struct PciDeviceNode {
    pci_device_t pub;
    dlist_head_t list_node;
};

struct PciDriverNode {
    pci_driver_t* pub;
    dlist_head_t list_node;
};

static void probe_bars(PciDeviceNode* node) {
    if (!node) {
        return;
    }

    pci_device_t* dev = &node->pub;

    for (uint8_t i = 0u; i < 6u; i++) {
        const uint8_t offset = static_cast<uint8_t>(0x10u + (i * 4u));

        uint32_t original = 0u;
        uint32_t size_mask = 0u;

        {
            kernel::ScopedIrqDisable irq_guard;

            original = ::pci_read(dev->bus, dev->slot, dev->func, offset);
            ::pci_write(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFFu);

            size_mask = ::pci_read(dev->bus, dev->slot, dev->func, offset);
            ::pci_write(dev->bus, dev->slot, dev->func, offset, original);
        }

        if (size_mask == 0u || size_mask == 0xFFFFFFFFu) {
            continue;
        }

        pci_bar_t* bar = &dev->bars[i];

        if ((original & 0x01u) == 0u) {
            bar->type = PCI_BAR_TYPE_MMIO;
            bar->base_addr = original & ~0x0Fu;
            bar->is_prefetchable = (original & 0x08u) != 0u;
            bar->is_64bit = ((original & 0x06u) == 0x04u);

            bar->size = ~(size_mask & ~0x0Fu) + 1u;

            if (bar->is_64bit && i + 1u < 6u) {
                i++;
            }
        } else {
            bar->type = PCI_BAR_TYPE_IO;
            bar->base_addr = original & ~0x03u;
            bar->is_prefetchable = 0u;
            bar->is_64bit = 0u;

            bar->size = ~(size_mask & ~0x03u) + 1u;
        }
    }
}

static bool match_id(const pci_device_t& dev, const pci_device_id_t& id) {
    if ((id.match_flags & PCI_MATCH_VENDOR_ID) != 0u
        && dev.vendor_id != id.vendor_id) {
        return false;
    }

    if ((id.match_flags & PCI_MATCH_DEVICE_ID_RANGE) != 0u) {
        if (dev.device_id < id.device_id || dev.device_id > id.device_id_last) {
            return false;
        }
    } else if ((id.match_flags & PCI_MATCH_DEVICE_ID) != 0u
        && dev.device_id != id.device_id) {
        return false;
    }

    if ((id.match_flags & PCI_MATCH_CLASS) != 0u
        && dev.class_code != id.class_code) {
        return false;
    }

    if ((id.match_flags & PCI_MATCH_SUBCLASS) != 0u
        && dev.subclass != id.subclass) {
        return false;
    }

    if ((id.match_flags & PCI_MATCH_PROG_IF) != 0u
        && dev.prog_if != id.prog_if) {
        return false;
    }

    return true;
}

static bool match_device_to_driver(const pci_device_t& dev, const pci_driver_t& drv) {
    if (!drv.id_table) {
        return false;
    }

    for (const pci_device_id_t* id = drv.id_table; id->match_flags != 0u; id++) {
        if (match_id(dev, *id)) {
            return true;
        }
    }

    return false;
}

class PciRegistry {
public:
    PciRegistry() = default;

    PciRegistry(const PciRegistry&) = delete;
    PciRegistry& operator=(const PciRegistry&) = delete;

    int register_driver(pci_driver_t* driver) {
        if (!driver || !driver->probe) {
            return -1;
        }

        ensure_init();

        PciDriverNode* node = new (kernel::nothrow) PciDriverNode();
        if (!node) {
            return -1;
        }

        node->pub = driver;

        kernel::SpinLockSafeGuard guard(lock_);

        drivers_.push_back(*node);

        for (PciDeviceNode& dev_node : devices_) {
            pci_device_t* dev = &dev_node.pub;

            if (dev->attached_driver != nullptr) {
                continue;
            }

            if (!match_device_to_driver(*dev, *driver)) {
                continue;
            }

            const int rc = driver->probe(dev);

            if (rc == 0) {
                dev->attached_driver = driver;

                kernel::output::kprintf(
                    "[pci] attached driver '%s' to %02x:%02x.%x\n",
                    driver->base.name ? driver->base.name : "unknown",
                    static_cast<uint32_t>(dev->bus),
                    static_cast<uint32_t>(dev->slot),
                    static_cast<uint32_t>(dev->func)
                );
            }
        }

        return 0;
    }

private:
    void ensure_init() {
        if (initialized_) {
            return;
        }

        kernel::SpinLockSafeGuard guard(lock_);

        if (initialized_) {
            return;
        }

        enumerate_buses_locked();

        initialized_ = true;
    }

    void enumerate_buses_locked() {
        for (uint16_t bus = 0u; bus < 256u; bus++) {
            for (uint8_t slot = 0u; slot < 32u; slot++) {
                for (uint8_t func = 0u; func < 8u; func++) {
                    probe_device_locked(
                        static_cast<uint8_t>(bus),
                        slot,
                        func
                    );

                    if (func == 0u) {
                        const uint32_t hdr_reg = ::pci_read(
                            static_cast<uint8_t>(bus),
                            slot,
                            func,
                            0x0Cu
                        );

                        const uint8_t header_type = static_cast<uint8_t>((hdr_reg >> 16) & 0xFFu);

                        if ((header_type & 0x80u) == 0u) {
                            break;
                        }
                    }
                }
            }
        }
    }

    void probe_device_locked(uint8_t bus, uint8_t slot, uint8_t func) {
        const uint32_t id_reg = ::pci_read(bus, slot, func, 0x00u);

        const uint16_t vendor = static_cast<uint16_t>(id_reg & 0xFFFFu);

        if (vendor == 0xFFFFu) {
            return;
        }

        PciDeviceNode* node = new (kernel::nothrow) PciDeviceNode();
        if (!node) {
            return;
        }

        memset(node, 0, sizeof(*node));

        pci_device_t* dev = &node->pub;

        dev->bus = bus;
        dev->slot = slot;
        dev->func = func;

        dev->vendor_id = vendor;
        dev->device_id = static_cast<uint16_t>((id_reg >> 16) & 0xFFFFu);

        const uint32_t class_reg = ::pci_read(bus, slot, func, 0x08u);

        dev->class_code = static_cast<uint8_t>((class_reg >> 24) & 0xFFu);
        dev->subclass = static_cast<uint8_t>((class_reg >> 16) & 0xFFu);
        dev->prog_if = static_cast<uint8_t>((class_reg >> 8) & 0xFFu);
        dev->revision = static_cast<uint8_t>(class_reg & 0xFFu);

        const uint32_t intr_reg = ::pci_read(bus, slot, func, 0x3Cu);

        dev->irq_line = static_cast<uint8_t>(intr_reg & 0xFFu);
        dev->irq_pin = static_cast<uint8_t>((intr_reg >> 8) & 0xFFu);

        probe_bars(node);

        devices_.push_back(*node);

        kernel::output::kprintf(
            "[pci] found %04x:%04x at %02x:%02x.%x (class %02x)\n",
            static_cast<uint32_t>(dev->vendor_id),
            static_cast<uint32_t>(dev->device_id),
            static_cast<uint32_t>(dev->bus),
            static_cast<uint32_t>(dev->slot),
            static_cast<uint32_t>(dev->func),
            static_cast<uint32_t>(dev->class_code)
        );
    }

    bool initialized_ = false;

    kernel::SpinLock lock_;
    kernel::CDBLinkedList<PciDeviceNode, &PciDeviceNode::list_node> devices_;
    kernel::CDBLinkedList<PciDriverNode, &PciDriverNode::list_node> drivers_;
};

static PciRegistry g_registry;

}

extern "C" {

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t address = static_cast<uint32_t>(
        (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFCu) | 0x80000000u
    );

    outl(kernel::pci::kPciConfigAddr, address);
    
    return inl(kernel::pci::kPciConfigData);
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    const uint32_t address = static_cast<uint32_t>(
        (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFCu) | 0x80000000u
    );

    outl(kernel::pci::kPciConfigAddr, address);
    outl(kernel::pci::kPciConfigData, value);
}

static inline uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t reg = pci_read(bus, slot, func, offset & 0xFCu);
    
    return static_cast<uint8_t>((reg >> ((offset & 3u) * 8u)) & 0xFFu);
}

static inline uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t reg = pci_read(bus, slot, func, offset & 0xFCu);
    
    return static_cast<uint16_t>((reg >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static inline void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    const uint8_t aligned = offset & 0xFCu;
    uint32_t reg = pci_read(bus, slot, func, aligned);
    const uint32_t shift = static_cast<uint32_t>(offset & 2u) * 8u;
    
    reg &= ~(0xFFFFu << shift);
    reg |= (static_cast<uint32_t>(value) << shift);
    
    pci_write(bus, slot, func, aligned, reg);
}

static inline void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    const uint8_t aligned = offset & 0xFCu;
    uint32_t reg = pci_read(bus, slot, func, aligned);
    const uint32_t shift = static_cast<uint32_t>(offset & 3u) * 8u;
    
    reg &= ~(0xFFu << shift);
    reg |= (static_cast<uint32_t>(value) << shift);
    
    pci_write(bus, slot, func, aligned, reg);
}

static void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    const uint32_t command = pci_read(bus, slot, func, 0x04u);

    if ((command & 0x05u) != 0x05u) {
        pci_write(bus, slot, func, 0x04u, command | 0x05u);
    }
}

uint32_t pci_get_bar4(uint8_t bus, uint8_t slot, uint8_t func) {
    const uint32_t bar4 = pci_read(bus, slot, func, 0x20u);
    
    return bar4 & 0xFFFFFFFCu;
}

uint32_t pci_get_bar5(uint8_t bus, uint8_t slot, uint8_t func) {
    const uint32_t bar5 = pci_read(bus, slot, func, 0x24u);
    
    return bar5 & 0xFFFFFFF0u;
}

int pci_msi_configure(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector, uint8_t dest_apic_id) {
    const uint32_t cmdsts = pci_read(bus, slot, func, 0x04u);
    const uint16_t status = static_cast<uint16_t>((cmdsts >> 16) & 0xFFFFu);

    if ((status & 0x0010u) == 0u) {
        return 0;
    }

    uint8_t cap = pci_read8(bus, slot, func, 0x34u);

    for (int iter = 0; iter < 48 && cap != 0; iter++) {
        const uint8_t cap_id = pci_read8(bus, slot, func, cap + 0);
        const uint8_t cap_next = pci_read8(bus, slot, func, cap + 1);

        if (cap_id == kernel::pci::kPciCapIdMsi) {
            uint16_t control = pci_read16(bus, slot, func, cap + 2);
            const int is_64 = (control & (1u << 7)) != 0u;

            const uint32_t msg_addr = 0xFEE00000u | (static_cast<uint32_t>(dest_apic_id) << 12);
            pci_write(bus, slot, func, cap + 4, msg_addr);

            const uint8_t data_off = static_cast<uint8_t>(cap + (is_64 ? 12u : 8u));
            if (is_64) {
                pci_write(bus, slot, func, cap + 8, 0u);
            }

            const uint16_t msg_data = static_cast<uint16_t>(vector);
            pci_write16(bus, slot, func, data_off, msg_data);

            control &= static_cast<uint16_t>(~(0x07u << 4));
            control |= 0x01u;
            pci_write16(bus, slot, func, cap + 2, control);

            uint16_t command = static_cast<uint16_t>(cmdsts & 0xFFFFu);
            command |= (1u << 10);
            pci_write16(bus, slot, func, 0x04u, command);

            return 1;
        }

        cap = cap_next;
    }

    return 0;
}

static int pci_msix_configure(
    pci_device_t* dev,
    uint16_t entry,
    uint8_t vector,
    uint8_t dest_apic_id
) {
    if (!dev) {
        return 0;
    }

    const uint32_t cmdsts = pci_dev_read32(dev, 0x04u);
    const uint16_t status = static_cast<uint16_t>((cmdsts >> 16) & 0xFFFFu);

    if ((status & 0x0010u) == 0u) {
        return 0;
    }

    uint8_t cap = pci_dev_read8(dev, 0x34u);

    for (int iter = 0; iter < 64 && cap != 0u; iter++) {
        const uint8_t cap_id = pci_dev_read8(dev, static_cast<uint8_t>(cap + 0u));
        const uint8_t cap_next = pci_dev_read8(dev, static_cast<uint8_t>(cap + 1u));

        if (cap_id != kernel::pci::kPciCapIdMsix) {
            cap = cap_next;
            continue;
        }

        uint16_t msg_ctl = pci_dev_read16(dev, static_cast<uint8_t>(cap + 2u));

        const uint16_t table_size = static_cast<uint16_t>((msg_ctl & 0x07FFu) + 1u);
        if (entry >= table_size) {
            return 0;
        }

        const uint32_t table = pci_dev_read32(dev, static_cast<uint8_t>(cap + 4u));
        const uint8_t bir = static_cast<uint8_t>(table & 0x7u);
        const uint32_t table_off = table & ~0x7u;

        __iomem* table_io = pci_request_bar(dev, bir, "pci_msix");
        if (!table_io) {
            return 0;
        }

        const uint32_t entry_off = table_off + (static_cast<uint32_t>(entry) * 16u);

        const uint32_t msg_addr_lo = 0xFEE00000u | (static_cast<uint32_t>(dest_apic_id) << 12);
        const uint32_t msg_addr_hi = 0u;
        const uint32_t msg_data = static_cast<uint32_t>(vector);

        iowrite32(table_io, entry_off + 12u, 1u);
        __sync_synchronize();

        iowrite32(table_io, entry_off + 0u, msg_addr_lo);
        iowrite32(table_io, entry_off + 4u, msg_addr_hi);
        iowrite32(table_io, entry_off + 8u, msg_data);
        __sync_synchronize();

        iowrite32(table_io, entry_off + 12u, 0u);
        __sync_synchronize();

        iomem_free(table_io);

        msg_ctl &= static_cast<uint16_t>(~(1u << 14));
        msg_ctl |= static_cast<uint16_t>(1u << 15);
        pci_dev_write16(dev, static_cast<uint8_t>(cap + 2u), msg_ctl);

        uint16_t command = static_cast<uint16_t>(cmdsts & 0xFFFFu);
        command |= static_cast<uint16_t>(1u << 10);
        pci_dev_write16(dev, 0x04u, command);

        return 1;
    }

    return 0;
}

int pci_register_driver(pci_driver_t* driver) {
    return kernel::pci::g_registry.register_driver(driver);
}

int pci_request_irq(pci_device_t* dev, irq_handler_t handler, void* ctx) {
    if (!dev || !handler) {
        return 0;
    }

    const uint8_t irq_line = dev->irq_line;

    if (irq_line >= 16u) {
        return 0;
    }

    irq_install_handler((int)irq_line, handler, ctx);

    if (ioapic_is_initialized() && cpu_count > 0 && cpus[0].id >= 0) {
        uint32_t gsi = 0u;
        int active_low = 0;
        int level_trigger = 0;

        if (!acpi_get_iso(irq_line, &gsi, &active_low, &level_trigger)) {
            gsi = (uint32_t)irq_line;
            active_low = 0;
            level_trigger = 0;
        }

        return ioapic_route_gsi(
            gsi,
            (uint8_t)(32u + irq_line),
            (uint8_t)cpus[0].id,
            active_low,
            level_trigger
        );
    }

    return pic_unmask_irq(irq_line);
}

uint32_t pci_dev_read32(const pci_device_t* dev, uint8_t offset) {
    if (!dev) {
        return 0u;
    }

    return pci_read(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write32(pci_device_t* dev, uint8_t offset, uint32_t value) {
    if (!dev) {
        return;
    }

    pci_write(dev->bus, dev->slot, dev->func, offset, value);
}

uint16_t pci_dev_read16(const pci_device_t* dev, uint8_t offset) {
    if (!dev) {
        return 0u;
    }

    return pci_read16(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write16(pci_device_t* dev, uint8_t offset, uint16_t value) {
    if (!dev) {
        return;
    }

    pci_write16(dev->bus, dev->slot, dev->func, offset, value);
}

uint8_t pci_dev_read8(const pci_device_t* dev, uint8_t offset) {
    if (!dev) {
        return 0u;
    }

    return pci_read8(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write8(pci_device_t* dev, uint8_t offset, uint8_t value) {
    if (!dev) {
        return;
    }

    pci_write8(dev->bus, dev->slot, dev->func, offset, value);
}

void pci_dev_enable_busmaster(pci_device_t* dev) {
    if (!dev) {
        return;
    }

    pci_enable_bus_master(dev->bus, dev->slot, dev->func);
}

int pci_dev_enable_msi(pci_device_t* dev, uint8_t vector, uint8_t dest_apic_id) {
    if (!dev) {
        return 0;
    }

    return pci_msi_configure(dev->bus, dev->slot, dev->func, vector, dest_apic_id);
}

int pci_dev_enable_msix(pci_device_t* dev, uint16_t entry, uint8_t vector, uint8_t dest_apic_id) {
    return pci_msix_configure(dev, entry, vector, dest_apic_id);
}

__iomem* pci_request_bar(pci_device_t* dev, uint8_t bar_idx, const char* name) {
    if (!dev || bar_idx >= 6u) {
        return nullptr;
    }

    const pci_bar_t* bar = &dev->bars[bar_idx];

    if (bar->base_addr == 0u || bar->size == 0u) {
        return nullptr;
    }

    if (bar->type == PCI_BAR_TYPE_MMIO) {
        return iomem_request_mmio(bar->base_addr, bar->size, name);
    }
    
    if (bar->type == PCI_BAR_TYPE_IO) {
        if (bar->base_addr > 0xFFFFu || bar->size > 0xFFFFu) {
            return nullptr;
        }

        return iomem_request_pmio((uint16_t)bar->base_addr, (uint16_t)bar->size, name);
    }

    return nullptr;
}


}
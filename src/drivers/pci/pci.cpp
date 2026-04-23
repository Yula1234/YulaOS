/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/new.h>
#include <lib/string.h>

#include <kernel/output/kprintf.h>
#include <kernel/smp/cpu.h>
#include <kernel/smp/mb.h>

#include <mm/heap.h>
#include <mm/vmm.h>

#include <arch/i386/paging.h>

#include <hal/io.h>

#include "pci.h"

extern "C" {
#include <drivers/acpi.h>
#include <hal/ioapic.h>
#include <hal/pic.h>

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t value);
}

namespace kernel::pci {

constexpr uint32_t kPciConfigAddr = 0xCF8u;
constexpr uint32_t kPciConfigData = 0xCFCu;

constexpr uint8_t kPciCapIdMsi  = 0x05u;
constexpr uint8_t kPciCapIdMsix = 0x11u;

static uint64_t g_mcfg_base = 0u;
static uint32_t g_mcfg_vaddr = 0u;
static uint8_t  g_mcfg_start_bus = 0u;
static uint8_t  g_mcfg_end_bus = 0u;
static bool     g_mcfg_enabled = false;

static void ensure_ecam_page_mapped(uint32_t offset_in_ecam) {
    const uint32_t page_offset = offset_in_ecam & ~0xFFFu;
    const uint32_t vaddr = g_mcfg_vaddr + page_offset;
    const uint32_t paddr = static_cast<uint32_t>(g_mcfg_base) + page_offset;

    uint32_t pte = 0u;

    if (!paging_get_present_pte(kernel_page_directory, vaddr, &pte)) {
        paging_map_ex(
            kernel_page_directory, vaddr, paddr,
            PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT,
            PAGING_MAP_NO_TLB_FLUSH
        );
    }
}

static volatile uint32_t* get_ecam_ptr(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    if (!g_mcfg_enabled) {
        return nullptr;
    }

    if (bus < g_mcfg_start_bus || bus > g_mcfg_end_bus) {
        return nullptr;
    }

    const uint32_t bus_offset  = static_cast<uint32_t>(bus - g_mcfg_start_bus) << 20u;
    const uint32_t slot_offset = static_cast<uint32_t>(slot) << 15u;
    const uint32_t func_offset = static_cast<uint32_t>(func) << 12u;
    const uint32_t reg_offset  = static_cast<uint32_t>(offset & ~3u);

    const uint32_t ecam_offset = bus_offset + slot_offset + func_offset + reg_offset;

    ensure_ecam_page_mapped(ecam_offset);

    return reinterpret_cast<volatile uint32_t*>(g_mcfg_vaddr + ecam_offset);
}

static uint8_t find_capability(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id) {
    const uint32_t cmdsts = ::pci_read(bus, slot, func, 0x04u);
    const uint16_t status = static_cast<uint16_t>((cmdsts >> 16) & 0xFFFFu);

    if ((status & 0x0010u) == 0u) {
        return 0u;
    }

    const uint32_t cap_ptr_reg = ::pci_read(bus, slot, func, 0x34u);
    uint8_t cap_offset = static_cast<uint8_t>(cap_ptr_reg & 0xFFu);

    for (int iter = 0; iter < 48 && cap_offset != 0u; iter++) {
        const uint32_t cap_reg = ::pci_read(bus, slot, func, cap_offset);
        
        const uint8_t current_cap_id = static_cast<uint8_t>(cap_reg & 0xFFu);
        const uint8_t next_cap_offset = static_cast<uint8_t>((cap_reg >> 8) & 0xFFu);

        if (current_cap_id == cap_id) {
            return cap_offset;
        }

        cap_offset = next_cap_offset;
    }

    return 0u;
}

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
        const uint16_t offset = static_cast<uint16_t>(0x10u + (i * 4u));

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

        uint16_t seg_group = 0u;
        
        if (acpi_get_mcfg(&g_mcfg_base, &seg_group, &g_mcfg_start_bus, &g_mcfg_end_bus)) {
            if (g_mcfg_base < 0xFFFFFFFFull) {
                const uint32_t ecam_size_bytes = (g_mcfg_end_bus - g_mcfg_start_bus + 1u) * 1048576u;
                const uint32_t ecam_pages = ecam_size_bytes / 4096u;

                void* reserved_vaddr = vmm_reserve_pages(ecam_pages);
                
                if (reserved_vaddr) {
                    g_mcfg_vaddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reserved_vaddr));
                    g_mcfg_enabled = true;
                    
                    kernel::output::kprintf(
                        "[pci] PCIe ECAM enabled at phys 0x%x, mapped to virt 0x%x (buses %u-%u)\n",
                        static_cast<uint32_t>(g_mcfg_base),
                        g_mcfg_vaddr,
                        g_mcfg_start_bus,
                        g_mcfg_end_bus
                    );
                } else {
                    kernel::output::kprintf("[pci] PCIe ECAM ignored: failed to reserve VMM space\n");
                }
            } else {
                kernel::output::kprintf("[pci] PCIe ECAM ignored: base address > 4GB\n");
            }
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
                        slot, func
                    );

                    if (func == 0u) {
                        const uint32_t hdr_reg = ::pci_read(
                            static_cast<uint8_t>(bus),
                            slot, func, 0x0Cu
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

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    volatile uint32_t* ecam_ptr = kernel::pci::get_ecam_ptr(bus, slot, func, offset);

    if (kernel::likely(ecam_ptr != nullptr)) {
        return *ecam_ptr;
    }

    const uint32_t address = static_cast<uint32_t>(
        (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFCu) | 0x80000000u
    );

    outl(kernel::pci::kPciConfigAddr, address);
    
    return inl(kernel::pci::kPciConfigData);
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t value) {
    volatile uint32_t* ecam_ptr = kernel::pci::get_ecam_ptr(bus, slot, func, offset);

    if (kernel::likely(ecam_ptr != nullptr)) {
        *ecam_ptr = value;
        return;
    }

    const uint32_t address = static_cast<uint32_t>(
        (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFCu) | 0x80000000u
    );

    outl(kernel::pci::kPciConfigAddr, address);
    outl(kernel::pci::kPciConfigData, value);
}

static inline uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    const uint32_t reg = pci_read(bus, slot, func, offset & 0xFFFCu);
    
    return static_cast<uint8_t>((reg >> ((offset & 3u) * 8u)) & 0xFFu);
}

static inline uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    const uint32_t reg = pci_read(bus, slot, func, offset & 0xFFFCu);
    
    return static_cast<uint16_t>((reg >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static inline void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint16_t value) {
    const uint16_t aligned = offset & 0xFFFCu;

    uint32_t reg = pci_read(bus, slot, func, aligned);

    const uint32_t shift = static_cast<uint32_t>(offset & 2u) * 8u;
    
    reg &= ~(0xFFFFu << shift);
    reg |= (static_cast<uint32_t>(value) << shift);
    
    pci_write(bus, slot, func, aligned, reg);
}

static inline void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint8_t value) {
    const uint16_t aligned = offset & 0xFFFCu;

    uint32_t reg = pci_read(bus, slot, func, aligned);

    const uint32_t shift = static_cast<uint32_t>(offset & 3u) * 8u;
    
    reg &= ~(0xFFu << shift);
    reg |= (static_cast<uint32_t>(value) << shift);
    
    pci_write(bus, slot, func, aligned, reg);
}

void pci_dev_enable_busmaster(pci_device_t* dev) {
    if (!dev) {
        return;
    }

    const uint32_t command = pci_read(dev->bus, dev->slot, dev->func, 0x04u);

    if ((command & 0x05u) != 0x05u) {
        pci_write(dev->bus, dev->slot, dev->func, 0x04u, command | 0x05u);
    }
}

int pci_msi_configure(uint8_t bus, uint8_t slot, uint8_t func, uint8_t vector, uint8_t dest_apic_id) {
    const uint8_t cap_offset = kernel::pci::find_capability(bus, slot, func, kernel::pci::kPciCapIdMsi);

    if (cap_offset == 0u) {
        return 0;
    }

    uint16_t control = pci_read16(bus, slot, func, static_cast<uint16_t>(cap_offset + 2u));
    const bool is_64bit = (control & (1u << 7)) != 0u;

    const uint32_t msg_addr = 0xFEE00000u | (static_cast<uint32_t>(dest_apic_id) << 12u);
    pci_write(bus, slot, func, static_cast<uint16_t>(cap_offset + 4u), msg_addr);

    const uint16_t data_offset = static_cast<uint16_t>(cap_offset + (is_64bit ? 12u : 8u));

    if (is_64bit) {
        pci_write(bus, slot, func, static_cast<uint16_t>(cap_offset + 8u), 0u);
    }

    pci_write16(bus, slot, func, data_offset, static_cast<uint16_t>(vector));

    control &= static_cast<uint16_t>(~(0x07u << 4u));
    control |= 0x01u;
    pci_write16(bus, slot, func, static_cast<uint16_t>(cap_offset + 2u), control);

    const uint32_t cmdsts = pci_read(bus, slot, func, 0x04u);

    uint16_t command = static_cast<uint16_t>(cmdsts & 0xFFFFu);

    command |= (1u << 10u);

    pci_write16(bus, slot, func, 0x04u, command);

    return 1;
}

int pci_dev_enable_msix(pci_device_t* dev, uint16_t entry, uint8_t vector, uint8_t dest_apic_id) {
    if (!dev) {
        return 0;
    }

    const uint8_t cap = kernel::pci::find_capability(dev->bus, dev->slot, dev->func, kernel::pci::kPciCapIdMsix);
    
    if (cap == 0u) {
        return 0;
    }

    uint16_t msg_ctl = pci_read16(dev->bus, dev->slot, dev->func, static_cast<uint16_t>(cap + 2u));
    
    const uint16_t table_size = static_cast<uint16_t>((msg_ctl & 0x07FFu) + 1u);
    
    if (entry >= table_size) {
        return 0;
    }

    const uint32_t table_reg = pci_read(dev->bus, dev->slot, dev->func, static_cast<uint16_t>(cap + 4u));
    
    const uint8_t bir = static_cast<uint8_t>(table_reg & 0x07u);
    const uint32_t table_off = table_reg & ~0x07u;

    __iomem* table_io = pci_request_bar(dev, bir, "pci_msix");
    
    if (!table_io) {
        return 0;
    }

    const uint32_t entry_off = table_off + (static_cast<uint32_t>(entry) * 16u);

    const uint32_t msg_addr_lo = 0xFEE00000u | (static_cast<uint32_t>(dest_apic_id) << 12u);
    const uint32_t msg_addr_hi = 0u;
    const uint32_t msg_data = static_cast<uint32_t>(vector);

    iowrite32(table_io, entry_off + 12u, 1u);
    smp_wmb();

    iowrite32(table_io, entry_off + 0u, msg_addr_lo);
    iowrite32(table_io, entry_off + 4u, msg_addr_hi);
    iowrite32(table_io, entry_off + 8u, msg_data);
    smp_wmb();

    iowrite32(table_io, entry_off + 12u, 0u);
    smp_wmb();

    iomem_free(table_io);

    msg_ctl |= static_cast<uint16_t>(1u << 15u);
    pci_write16(dev->bus, dev->slot, dev->func, static_cast<uint16_t>(cap + 2u), msg_ctl);

    const uint32_t cmdsts = pci_read(dev->bus, dev->slot, dev->func, 0x04u);

    uint16_t command = static_cast<uint16_t>(cmdsts & 0xFFFFu);

    command |= static_cast<uint16_t>(1u << 10u);

    pci_write16(dev->bus, dev->slot, dev->func, 0x04u, command);

    return 1;
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
            gsi = static_cast<uint32_t>(irq_line);
            active_low = 0;
            level_trigger = 0;
        }

        return ioapic_route_gsi(
            gsi,
            static_cast<uint8_t>(32u + irq_line),
            static_cast<uint8_t>(cpus[0].id),
            active_low,
            level_trigger
        );
    }

    return pic_unmask_irq(irq_line);
}

uint32_t pci_dev_read32(const pci_device_t* dev, uint16_t offset) {
    if (!dev) {
        return 0u;
    }

    return pci_read(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write32(pci_device_t* dev, uint16_t offset, uint32_t value) {
    if (!dev) {
        return;
    }

    pci_write(dev->bus, dev->slot, dev->func, offset, value);
}

uint16_t pci_dev_read16(const pci_device_t* dev, uint16_t offset) {
    if (!dev) {
        return 0u;
    }

    return pci_read16(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write16(pci_device_t* dev, uint16_t offset, uint16_t value) {
    if (!dev) {
        return;
    }

    pci_write16(dev->bus, dev->slot, dev->func, offset, value);
}

uint8_t pci_dev_read8(const pci_device_t* dev, uint16_t offset) {
    if (!dev) {
        return 0u;
    }

    return pci_read8(dev->bus, dev->slot, dev->func, offset);
}

void pci_dev_write8(pci_device_t* dev, uint16_t offset, uint8_t value) {
    if (!dev) {
        return;
    }

    pci_write8(dev->bus, dev->slot, dev->func, offset, value);
}

int pci_dev_enable_msi(pci_device_t* dev, uint8_t vector, uint8_t dest_apic_id) {
    if (!dev) {
        return 0;
    }

    return pci_msi_configure(dev->bus, dev->slot, dev->func, vector, dest_apic_id);
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

        return iomem_request_pmio(static_cast<uint16_t>(bar->base_addr), static_cast<uint16_t>(bar->size), name);
    }

    return nullptr;
}

}
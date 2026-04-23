/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/acpi.h>

#include <arch/i386/paging.h>

#include <kernel/smp/cpu_limits.h>
#include <kernel/smp/cpu.h>
#include <kernel/panic.h>

#include <mm/pmm.h>

#include <hal/io.h>

#include <lib/compiler.h>
#include <lib/string.h>

typedef struct AcpiRsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) AcpiRsdp;

typedef struct AcpiSdtHeader {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) AcpiSdtHeader;

typedef struct AcpiGenericAddress {
    uint8_t  address_space;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed)) AcpiGenericAddress;

typedef struct AcpiMadt {
    AcpiSdtHeader header;
    uint32_t      local_apic_addr;
    uint32_t      flags;
} __attribute__((packed)) AcpiMadt;

typedef struct AcpiMadtEntryHeader {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) AcpiMadtEntryHeader;

typedef struct AcpiMadtProcessorApic {
    AcpiMadtEntryHeader header;
    uint8_t             acpi_processor_id;
    uint8_t             apic_id;
    uint32_t            flags;
} __attribute__((packed)) AcpiMadtProcessorApic;

typedef struct AcpiMadtIoApic {
    AcpiMadtEntryHeader header;
    uint8_t             ioapic_id;
    uint8_t             reserved;
    uint32_t            ioapic_addr;
    uint32_t            gsi_base;
} __attribute__((packed)) AcpiMadtIoApic;

typedef struct AcpiMadtIso {
    AcpiMadtEntryHeader header;
    uint8_t             bus;
    uint8_t             source_irq;
    uint32_t            gsi;
    uint16_t            flags;
} __attribute__((packed)) AcpiMadtIso;

typedef struct AcpiFadt {
    AcpiSdtHeader      header;
    uint32_t           firmware_ctrl;
    uint32_t           dsdt;
    uint8_t            reserved;
    uint8_t            preferred_pm_profile;
    uint16_t           sci_interrupt;
    uint32_t           smi_cmd;
    uint8_t            acpi_enable;
    uint8_t            acpi_disable;
    uint8_t            s4_bios_req;
    uint8_t            pstate_cnt;
    uint32_t           pm1a_evt_blk;
    uint32_t           pm1b_evt_blk;
    uint32_t           pm1a_cnt_blk;
    uint32_t           pm1b_cnt_blk;
    uint32_t           pm2_cnt_blk;
    uint32_t           pm_tmr_blk;
    uint32_t           gpe0_blk;
    uint32_t           gpe1_blk;
    uint8_t            pm1_evt_len;
    uint8_t            pm1_cnt_len;
    uint8_t            pm2_cnt_len;
    uint8_t            pm_tmr_len;
    uint8_t            gpe0_len;
    uint8_t            gpe1_len;
    uint8_t            gpe1_base;
    uint8_t            cst_cnt;
    uint16_t           p_lvl2_lat;
    uint16_t           p_lvl3_lat;
    uint16_t           flush_size;
    uint16_t           flush_stride;
    uint8_t            duty_offset;
    uint8_t            duty_width;
    uint8_t            day_alrm;
    uint8_t            mon_alrm;
    uint8_t            century;
    uint16_t           iapc_boot_arch;
    uint8_t            reserved2;
    uint32_t           flags;
    AcpiGenericAddress reset_reg;
    uint8_t            reset_value;
    uint8_t            reserved3[3];
} __attribute__((packed)) AcpiFadt;

typedef struct AcpiMcfgAllocation {
    uint64_t base_address;
    uint16_t pci_segment_group;
    uint8_t  start_bus_number;
    uint8_t  end_bus_number;
    uint32_t reserved;
} __attribute__((packed)) AcpiMcfgAllocation;

typedef struct AcpiMcfg {
    AcpiSdtHeader      header;
    uint64_t           reserved;
    AcpiMcfgAllocation allocations[];
} __attribute__((packed)) AcpiMcfg;

#define ACPI_GAS_SYSTEM_MEMORY 0u
#define ACPI_GAS_SYSTEM_IO     1u

static volatile int g_acpi_ready = 0;

static uint32_t g_ioapic_phys = 0u;
static uint32_t g_ioapic_gsi_base = 0u;
static int      g_have_ioapic = 0;

static uint32_t g_iso_gsi[16];
static uint8_t  g_iso_active_low[16];
static uint8_t  g_iso_level_trigger[16];

static uint32_t g_pm1a_cnt_blk = 0u;
static AcpiGenericAddress g_reset_reg;
static uint8_t  g_reset_value = 0u;
static int      g_have_fadt = 0;

static uint64_t g_mcfg_base = 0u;
static uint16_t g_mcfg_seg_group = 0u;
static uint8_t  g_mcfg_start_bus = 0u;
static uint8_t  g_mcfg_end_bus = 0u;
static int      g_have_mcfg = 0;

static void ensure_mapped_range(uint32_t phys_start, uint32_t length) {
    if (length == 0u) {
        return;
    }

    const uint32_t start_page = phys_start & ~0xFFFu;
    const uint32_t end_page   = (phys_start + length + 0xFFFu) & ~0xFFFu;

    for (uint32_t page = start_page; page < end_page; page += 0x1000u) {
        const uint32_t pde_idx = page >> 22;
        const uint32_t pte_idx = (page >> 12) & 0x3FFu;

        uint32_t* pd = kernel_page_directory;

        if ((pd[pde_idx] & 1u) == 0u) {
            paging_map(pd, page, page, 3u);
        } else {
            uint32_t* pt = (uint32_t*)(pd[pde_idx] & ~0xFFFu);
            
            if ((pt[pte_idx] & 1u) == 0u) {
                paging_map(pd, page, page, 3u);
            }
        }
    }
}

static void* map_acpi_table(uint64_t phys_addr_64) {
    if (unlikely(phys_addr_64 >= 0xFFFFFFFFull)) {
        return NULL;
    }

    const uint32_t phys = (uint32_t)phys_addr_64;

    ensure_mapped_range(phys, sizeof(AcpiSdtHeader));

    AcpiSdtHeader* header = (AcpiSdtHeader*)phys;

    if (header->length > sizeof(AcpiSdtHeader)) {
        ensure_mapped_range(phys, header->length);
    }

    return header;
}

static int verify_checksum(const uint8_t* ptr, size_t len) {
    if (unlikely(!ptr || len == 0)) {
        return 0;
    }

    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += ptr[i];
    }

    return sum == 0;
}

static AcpiRsdp* find_rsdp_legacy(void) {
    for (uint32_t addr = 0xE0000u; addr < 0x100000u; addr += 16u) {
        if (strncmp((char*)addr, "RSD PTR ", 8) == 0) {
            if (verify_checksum((uint8_t*)addr, 20u)) {
                return (AcpiRsdp*)addr;
            }
        }
    }

    return NULL;
}

static void parse_madt(AcpiMadt* madt) {
    if (unlikely(!madt)) {
        return;
    }

    for (int i = 0; i < 16; i++) {
        g_iso_gsi[i] = (uint32_t)i;
        g_iso_active_low[i] = 0;
        g_iso_level_trigger[i] = 0;
    }

    g_have_ioapic = 0;
    cpu_count = 0;

    uint8_t* ptr = (uint8_t*)madt + sizeof(AcpiMadt);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        AcpiMadtEntryHeader* entry = (AcpiMadtEntryHeader*)ptr;

        if (entry->length == 0u) {
            break;
        }

        if (entry->type == 0u) {
            AcpiMadtProcessorApic* proc = (AcpiMadtProcessorApic*)entry;

            if ((proc->flags & 1u) != 0u) {
                if (cpu_count < MAX_CPUS) {
                    cpus[cpu_count].id = proc->apic_id;
                    cpus[cpu_count].index = cpu_count;
                    cpus[cpu_count].started = 0;

                    cpu_count++;
                }
            }
        } else if (entry->type == 1u) {
            if (!g_have_ioapic && entry->length >= sizeof(AcpiMadtIoApic)) {
                AcpiMadtIoApic* ioa = (AcpiMadtIoApic*)entry;

                g_ioapic_phys = ioa->ioapic_addr;
                g_ioapic_gsi_base = ioa->gsi_base;

                g_have_ioapic = 1;
            }
        } else if (entry->type == 2u) {
            if (entry->length >= sizeof(AcpiMadtIso)) {
                AcpiMadtIso* iso = (AcpiMadtIso*)entry;

                if (iso->source_irq < 16u) {
                    g_iso_gsi[iso->source_irq] = iso->gsi;

                    const uint16_t pol = iso->flags & 0x03u;
                    const uint16_t trg = (iso->flags >> 2) & 0x03u;

                    if (pol == 3u) {
                        g_iso_active_low[iso->source_irq] = 1;
                    } else if (pol == 1u) {
                        g_iso_active_low[iso->source_irq] = 0;
                    }

                    if (trg == 3u) {
                        g_iso_level_trigger[iso->source_irq] = 1;
                    } else if (trg == 1u) {
                        g_iso_level_trigger[iso->source_irq] = 0;
                    }
                }
            }
        }

        ptr += entry->length;
    }
}

static void parse_fadt(AcpiFadt* fadt) {
    if (unlikely(!fadt)) {
        return;
    }

    g_pm1a_cnt_blk = fadt->pm1a_cnt_blk;

    g_reset_reg = fadt->reset_reg;
    g_reset_value = fadt->reset_value;

    g_have_fadt = 1;
}

static void parse_mcfg(AcpiMcfg* mcfg) {
    if (unlikely(!mcfg)) {
        return;
    }

    const uint32_t length = mcfg->header.length;
    const uint32_t min_expected = sizeof(AcpiMcfg) + sizeof(AcpiMcfgAllocation);

    if (length < min_expected) {
        return;
    }

    AcpiMcfgAllocation* alloc = &mcfg->allocations[0];

    g_mcfg_base = alloc->base_address;
    g_mcfg_seg_group = alloc->pci_segment_group;
    g_mcfg_start_bus = alloc->start_bus_number;
    g_mcfg_end_bus = alloc->end_bus_number;

    g_have_mcfg = 1;
}

void acpi_init(void) {
    if (unlikely(g_acpi_ready)) {
        return;
    }

    AcpiRsdp* rsdp = find_rsdp_legacy();

    if (!rsdp) {
        return;
    }

    int use_xsdt = 0;

    if (rsdp->revision >= 2u && rsdp->xsdt_addr != 0ull) {
        if (verify_checksum((uint8_t*)rsdp, sizeof(AcpiRsdp))) {
            use_xsdt = 1;
        }
    }

    AcpiSdtHeader* root_sdt = NULL;
    uint32_t entries_count = 0u;

    if (use_xsdt) {
        root_sdt = (AcpiSdtHeader*)map_acpi_table(rsdp->xsdt_addr);
        
        if (root_sdt && strncmp(root_sdt->signature, "XSDT", 4) == 0) {
            entries_count = (root_sdt->length - sizeof(AcpiSdtHeader)) / 8u;
        } else {
            root_sdt = NULL;
        }
    } 
    
    if (!root_sdt) {
        use_xsdt = 0;
        root_sdt = (AcpiSdtHeader*)map_acpi_table((uint64_t)rsdp->rsdt_addr);

        if (root_sdt && strncmp(root_sdt->signature, "RSDT", 4) == 0) {
            entries_count = (root_sdt->length - sizeof(AcpiSdtHeader)) / 4u;
        } else {
            return;
        }
    }

    uint8_t* ptr_array = (uint8_t*)root_sdt + sizeof(AcpiSdtHeader);

    for (uint32_t i = 0u; i < entries_count; i++) {
        uint64_t phys_addr = 0u;

        if (use_xsdt) {
            phys_addr = *(uint64_t*)(ptr_array + i * 8u);
        } else {
            phys_addr = (uint64_t)(*(uint32_t*)(ptr_array + i * 4u));
        }

        AcpiSdtHeader* header = (AcpiSdtHeader*)map_acpi_table(phys_addr);

        if (!header) {
            continue;
        }

        if (strncmp(header->signature, "APIC", 4) == 0) {
            parse_madt((AcpiMadt*)header);
        } else if (strncmp(header->signature, "FACP", 4) == 0) {
            parse_fadt((AcpiFadt*)header);
        } else if (strncmp(header->signature, "MCFG", 4) == 0) {
            parse_mcfg((AcpiMcfg*)header);
        }
    }

    g_acpi_ready = 1;
}

int acpi_get_ioapic(
    uint32_t* out_phys_addr,
    uint32_t* out_gsi_base
) {
    if (unlikely(!g_acpi_ready || !g_have_ioapic)) {
        return 0;
    }

    if (unlikely(!out_phys_addr || !out_gsi_base)) {
        return 0;
    }

    *out_phys_addr = g_ioapic_phys;
    *out_gsi_base  = g_ioapic_gsi_base;

    return 1;
}

int acpi_get_iso(
    uint8_t source_irq,
    uint32_t* out_gsi,
    int* out_active_low,
    int* out_level_trigger
) {
    if (unlikely(!g_acpi_ready)) {
        return 0;
    }

    if (unlikely(source_irq >= 16u)) {
        return 0;
    }

    if (unlikely(!out_gsi || !out_active_low || !out_level_trigger)) {
        return 0;
    }

    *out_gsi = g_iso_gsi[source_irq];
    *out_active_low = g_iso_active_low[source_irq] ? 1 : 0;
    *out_level_trigger = g_iso_level_trigger[source_irq] ? 1 : 0;

    return 1;
}

int acpi_get_mcfg(
    uint64_t* out_base_addr,
    uint16_t* out_pci_seg_group,
    uint8_t* out_start_bus,
    uint8_t* out_end_bus
) {
    if (unlikely(!g_acpi_ready || !g_have_mcfg)) {
        return 0;
    }

    if (unlikely(!out_base_addr || !out_pci_seg_group || !out_start_bus || !out_end_bus)) {
        return 0;
    }

    *out_base_addr     = g_mcfg_base;
    *out_pci_seg_group = g_mcfg_seg_group;
    *out_start_bus     = g_mcfg_start_bus;
    *out_end_bus       = g_mcfg_end_bus;

    return 1;
}

void acpi_reboot(void) {
    __asm__ volatile("cli");

    if (g_have_fadt) {
        if (g_reset_reg.address_space == ACPI_GAS_SYSTEM_IO) {
            outb((uint16_t)g_reset_reg.address, g_reset_value);
        } else if (g_reset_reg.address_space == ACPI_GAS_SYSTEM_MEMORY) {
            const uint32_t phys = (uint32_t)g_reset_reg.address;

            ensure_mapped_range(phys, 1u);

            volatile uint8_t* reg = (volatile uint8_t*)phys;
            *reg = g_reset_value;
        }
    }

    uint8_t s = inb(0x64u);
    
    while ((s & 0x02u) != 0u) {
        s = inb(0x64u);
    }
    
    outb(0x64u, 0xFEu);

    while (1) {
        __asm__ volatile("hlt");
    }
}

void acpi_poweroff(void) {
    __asm__ volatile("cli");

    if (g_have_fadt && g_pm1a_cnt_blk != 0u) {
        outw((uint16_t)g_pm1a_cnt_blk, 0x2000u);
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}
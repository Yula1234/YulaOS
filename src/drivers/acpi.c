// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <arch/i386/paging.h>
#include <kernel/panic.h>
#include <drivers/vga.h>
#include <mm/pmm.h>
#include <kernel/cpu.h>

#include "acpi.h"

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
} __attribute__((packed)) rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

typedef struct {
    sdt_header_t h;
    uint32_t local_apic_addr;
    uint32_t flags;
} __attribute__((packed)) madt_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_header_t;

typedef struct {
    madt_entry_header_t h;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags; // Bit 0 = Processor Enabled
} __attribute__((packed)) madt_processor_apic_t;

typedef struct {
    madt_entry_header_t h;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed)) madt_ioapic_t;

typedef struct {
    madt_entry_header_t h;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed)) madt_iso_t;

static volatile int g_acpi_ready = 0;
static uint32_t g_ioapic_phys = 0;
static uint32_t g_ioapic_gsi_base = 0;
static int g_have_ioapic = 0;

static uint32_t g_iso_gsi[16];
static uint8_t g_iso_active_low[16];
static uint8_t g_iso_level_trigger[16];

static void ensure_mapped(uint32_t phys_addr) {
    uint32_t vaddr = phys_addr; 
    uint32_t* pd = kernel_page_directory;
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;
    
    if (!(pd[pde_idx] & 1)) {
        paging_map(pd, vaddr, vaddr, 3);
    } else {
        uint32_t* pt = (uint32_t*)(pd[pde_idx] & ~0xFFF);
        if (!(pt[pte_idx] & 1)) {
            paging_map(pd, vaddr, vaddr, 3);
        }
    }
}

static int check_sum(uint8_t* ptr, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += ptr[i];
    return sum == 0;
}

void acpi_init(void) {
    rsdp_t* rsdp = 0;
    
    for (uint32_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        if (strncmp((char*)addr, "RSD PTR ", 8) == 0) {
            if (check_sum((uint8_t*)addr, sizeof(rsdp_t))) {
                rsdp = (rsdp_t*)addr;
                break;
            }
        }
    }

    if (!rsdp) {
        return; 
    }

    ensure_mapped(rsdp->rsdt_addr);
    sdt_header_t* rsdt = (sdt_header_t*)rsdp->rsdt_addr;
    
    if (strncmp(rsdt->signature, "RSDT", 4) != 0) {
        return;
    }

    int entries = (rsdt->length - sizeof(sdt_header_t)) / 4;
    uint32_t* pointers = (uint32_t*)((uint8_t*)rsdt + sizeof(sdt_header_t));
    
    madt_t* madt = 0;

    for (int i = 0; i < entries; i++) {
        ensure_mapped(pointers[i]);
        sdt_header_t* header = (sdt_header_t*)pointers[i];
        if (strncmp(header->signature, "APIC", 4) == 0) {
            madt = (madt_t*)header;
            break;
        }
    }

    if (!madt) return;

    for (int i = 0; i < 16; i++) {
        g_iso_gsi[i] = (uint32_t)i;
        g_iso_active_low[i] = 0;
        g_iso_level_trigger[i] = 0;
    }
    g_have_ioapic = 0;

    cpu_count = 0;
    
    uint8_t* ptr = (uint8_t*)madt + sizeof(madt_t);
    uint8_t* end = (uint8_t*)madt + madt->h.length;

    while (ptr < end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;
        
        if (entry->type == 0) {
            madt_processor_apic_t* proc = (madt_processor_apic_t*)entry;
            
            if (proc->flags & 1) {
                if (cpu_count < MAX_CPUS) {
                    cpus[cpu_count].id = proc->apic_id;
                    cpus[cpu_count].index = cpu_count;
                    cpus[cpu_count].started = 0;
                    cpu_count++;
                }
            }
        }

        if (entry->type == 1) {
            if (!g_have_ioapic && entry->length >= sizeof(madt_ioapic_t)) {
                madt_ioapic_t* ioa = (madt_ioapic_t*)entry;
                g_ioapic_phys = ioa->ioapic_addr;
                g_ioapic_gsi_base = ioa->gsi_base;
                g_have_ioapic = 1;
            }
        }

        if (entry->type == 2) {
            if (entry->length >= sizeof(madt_iso_t)) {
                madt_iso_t* iso = (madt_iso_t*)entry;
                if (iso->source_irq < 16) {
                    g_iso_gsi[iso->source_irq] = iso->gsi;

                    uint16_t pol = iso->flags & 0x3;
                    uint16_t trg = (iso->flags >> 2) & 0x3;

                    if (pol == 3) g_iso_active_low[iso->source_irq] = 1;
                    else if (pol == 1) g_iso_active_low[iso->source_irq] = 0;

                    if (trg == 3) g_iso_level_trigger[iso->source_irq] = 1;
                    else if (trg == 1) g_iso_level_trigger[iso->source_irq] = 0;
                }
            }
        }
        
        ptr += entry->length;
    }

    g_acpi_ready = 1;
}

int acpi_get_ioapic(uint32_t* out_phys_addr, uint32_t* out_gsi_base) {
    if (!g_acpi_ready || !g_have_ioapic) return 0;
    if (!out_phys_addr || !out_gsi_base) return 0;
    *out_phys_addr = g_ioapic_phys;
    *out_gsi_base = g_ioapic_gsi_base;
    return 1;
}

int acpi_get_iso(uint8_t source_irq, uint32_t* out_gsi, int* out_active_low, int* out_level_trigger) {
    if (!g_acpi_ready) return 0;
    if (source_irq >= 16) return 0;
    if (!out_gsi || !out_active_low || !out_level_trigger) return 0;
    *out_gsi = g_iso_gsi[source_irq];
    *out_active_low = g_iso_active_low[source_irq] ? 1 : 0;
    *out_level_trigger = g_iso_level_trigger[source_irq] ? 1 : 0;
    return 1;
}
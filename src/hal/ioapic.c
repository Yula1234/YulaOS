// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>

#include <arch/i386/paging.h>

#include "ioapic.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WIN    0x10

#define IOAPICID   0x00
#define IOAPICVER  0x01
#define IOAPICARB  0x02
#define IOREDTBL_BASE 0x10

static volatile uint32_t* g_ioapic_mmio = 0;
static uint32_t g_ioapic_gsi_base = 0;
static uint32_t g_ioapic_max_redir = 0;
static int g_ioapic_inited = 0;

static inline void ioapic_write_reg(uint8_t reg, uint32_t val) {
    if (!g_ioapic_mmio) return;
    volatile uint32_t* regsel = (volatile uint32_t*)((uintptr_t)g_ioapic_mmio + IOAPIC_REGSEL);
    volatile uint32_t* win    = (volatile uint32_t*)((uintptr_t)g_ioapic_mmio + IOAPIC_WIN);
    *regsel = reg;
    *win = val;
}

static inline uint32_t ioapic_read_reg(uint8_t reg) {
    if (!g_ioapic_mmio) return 0;
    volatile uint32_t* regsel = (volatile uint32_t*)((uintptr_t)g_ioapic_mmio + IOAPIC_REGSEL);
    volatile uint32_t* win    = (volatile uint32_t*)((uintptr_t)g_ioapic_mmio + IOAPIC_WIN);
    *regsel = reg;
    return *win;
}

static inline void ioapic_write_redir(uint32_t index, uint32_t low, uint32_t high) {
    uint32_t reg_low  = IOREDTBL_BASE + (index * 2);
    uint32_t reg_high = IOREDTBL_BASE + (index * 2) + 1;
    ioapic_write_reg((uint8_t)reg_high, high);
    ioapic_write_reg((uint8_t)reg_low, low);
}

static inline uint32_t ioapic_read_redir_low(uint32_t index) {
    uint32_t reg_low  = IOREDTBL_BASE + (index * 2);
    return ioapic_read_reg((uint8_t)reg_low);
}

int ioapic_is_initialized(void) {
    return g_ioapic_inited;
}

int ioapic_init(uint32_t phys_addr, uint32_t gsi_base) {
    if (phys_addr == 0) return 0;

    uint32_t page = phys_addr & ~0xFFFu;
    paging_map(kernel_page_directory, page, page, 0x13);

    g_ioapic_mmio = (volatile uint32_t*)(uintptr_t)phys_addr;
    g_ioapic_gsi_base = gsi_base;

    uint32_t ver = ioapic_read_reg(IOAPICVER);
    g_ioapic_max_redir = (ver >> 16) & 0xFF;

    g_ioapic_inited = 1;
    return 1;
}

int ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id, int active_low, int level_trigger) {
    if (!g_ioapic_inited) return 0;
    if (gsi < g_ioapic_gsi_base) return 0;

    uint32_t index = gsi - g_ioapic_gsi_base;
    if (index > g_ioapic_max_redir) return 0;

    uint32_t low = 0;
    uint32_t high = 0;

    low |= (uint32_t)vector;

    if (active_low) {
        low |= (1u << 13);
    }

    if (level_trigger) {
        low |= (1u << 15);
    }

    high |= ((uint32_t)dest_apic_id) << 24;
    uint32_t old_low = ioapic_read_redir_low(index);
    ioapic_write_redir(index, old_low | (1u << 16), high);
    ioapic_write_redir(index, low | (1u << 16), high);
    ioapic_write_redir(index, low & ~(1u << 16), high);
    return 1;
}

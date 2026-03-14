/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */
  
#include <lib/string.h>
#include <kernel/smp/cpu.h>
#include "gdt.h"

/*
 * GDT segment descriptor (8 bytes).
 *
 * base and limit are split across fields. granularity packs:
 *  - high 4 bits of limit
 *  - flags (G, D/B, L, AVL)
 *
 * access encodes present bit, DPL, descriptor type, and segment type.
 */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* GDTR operand for lgdt: limit is (bytes-1). */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct gdt_entry gdt[GDT_ENTRIES];
struct gdt_ptr gp;

struct tss_entry_struct tss_entries[MAX_CPUS];

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    /*
     * `limit` is interpreted according to granularity flags.
     * For flat segments we use 0xFFFFFFFF with G=1 and D=1.
     */
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

void gdt_load(void) {
    /*
     * After lgdt, segment registers keep cached descriptor state.
     * Reload them and use a far jump to flush CS.
     */
    __asm__ volatile("lgdt %0" : : "m" (gp));
    
    __asm__ volatile(
        "mov $0x10, %%ax \n"
        "mov %%ax, %%ds \n"
        "mov %%ax, %%es \n"
        "mov %%ax, %%fs \n"
        "mov %%ax, %%gs \n"
        "mov %%ax, %%ss \n"
        "ljmp $0x08, $1f \n"  
        "1: \n"             
        : : : "eax"
    );
}

void gdt_init() {
    /*
     * Standard flat model:
     *  0: null
     *  1: kernel code (0x08)
     *  2: kernel data (0x10)
     *  3: user code   (0x18)
     *  4: user data   (0x20)
     *  5..: per-CPU TSS descriptors
     */
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);               
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    
    for (int i = 0; i < MAX_CPUS; i++) {
        uint32_t base = (uint32_t) &tss_entries[i];
        uint32_t limit = sizeof(struct tss_entry_struct) - 1;

        /* Available 32-bit TSS (system segment), present, DPL=0. */
        gdt_set_gate(5 + i, base, limit, 0x89, 0x00);

        memset(&tss_entries[i], 0, sizeof(struct tss_entry_struct));
        
        tss_entries[i].ss0  = 0x10;
        tss_entries[i].iomap_base = sizeof(struct tss_entry_struct);
    }

    gdt_load();
    
    /*
     * Load TSS selector.
     * This points to the first TSS descriptor (index 5 => selector 0x28).
     * The TSS selector is used to store the ESP0 value, which is used by the
     * CPU on CPL3->CPL0 transition (interrupt/syscall).
     */
    __asm__ volatile("ltr %%ax" : : "a" (0x28));
}

/* Update esp0 for a given CPU's TSS. */
void tss_set_stack(int cpu_id, uint32_t kernel_esp) {
    /* esp0 is used by CPU on CPL3->CPL0 transition (interrupt/syscall). */
    if (cpu_id >= 0 && cpu_id < MAX_CPUS) {
        tss_entries[cpu_id].esp0 = kernel_esp;
    }
}
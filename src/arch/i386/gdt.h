// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ARCH_I386_GDT_H
#define ARCH_I386_GDT_H

#include <stdint.h>
#include <kernel/cpu.h>

#define GDT_ENTRIES (5 + MAX_CPUS) 

struct tss_entry_struct {
    uint32_t prev_tss;
    uint32_t esp0; 
    uint32_t ss0;
    uint32_t esp1; uint32_t ss1;
    uint32_t esp2; uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax; uint32_t ecx; uint32_t edx; uint32_t ebx;
    uint32_t esp; uint32_t ebp; uint32_t esi; uint32_t edi;
    uint32_t es;  uint32_t cs;  uint32_t ss;  uint32_t ds;  uint32_t fs;  uint32_t gs;
    uint32_t ldt; uint16_t trap; uint16_t iomap_base;
} __attribute__((packed));

extern struct tss_entry_struct tss_entries[MAX_CPUS];

#ifdef __cplusplus
extern "C" {
#endif

void gdt_init(void);
void gdt_load(void);
void tss_set_stack(int cpu_id, uint32_t kernel_esp);

#ifdef __cplusplus
}
#endif

#endif
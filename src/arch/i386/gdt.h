/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef ARCH_I386_GDT_H
#define ARCH_I386_GDT_H

#include <kernel/smp/cpu.h>

#include <hal/align.h>

#include <stdint.h>

/*
 * Global Descriptor Table layout.
 *
 * The GDT contains the flat kernel/user code+data segments, per-CPU TSS,
 * and per-CPU data segments for GS-based cpu_current() access.
 */
#define GDT_ENTRIES (5 + MAX_CPUS * 3)

/* Base index for per-CPU data segments (GS selectors). */
#define GDT_CPU_DATA_BASE (5 + MAX_CPUS) 
#define GDT_USER_TLS_BASE (5 + MAX_CPUS * 2)

/*
 * i386 TSS.
 *
 * Only a small subset is actively used by the kernel:
 *  - esp0/ss0: ring transition stack (userspace -> kernel)
 *  - iomap_base: set past the structure to disable the I/O bitmap
 *
 * The layout is ABI with the CPU; keep packed.
 */
struct __cacheline_aligned tss_entry_struct {
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

extern __cacheline_aligned struct tss_entry_struct tss_entries[MAX_CPUS];

#ifdef __cplusplus
extern "C" {
#endif

/* Build and load GDT, and install/load initial TSS selector. */
void gdt_init(void);

/* Load GDTR and reload segment registers. */
void gdt_load(void);

/* Update esp0 for a given CPU's TSS. */
void tss_set_stack(int cpu_id, uint32_t kernel_esp);

void gdt_set_user_tls(int cpu_id, uint32_t base);

#ifdef __cplusplus
}
#endif

#endif
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef ARCH_I386_IDT_H
#define ARCH_I386_IDT_H

#include <stdint.h>

/*
 * i386 interrupt/exception entry ABI.
 *
 * registers_t mirrors the stack frame built by the ISR stubs.
 * Layout is ABI: any change here must be mirrored in the assembly stubs.
 */
typedef struct {
    uint32_t gs, fs, es, ds; 
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; 
    uint32_t int_no, err_code; 
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed)) registers_t;

/* Wall clock seconds since boot (driven by the timer IRQ). */
extern volatile uint32_t system_uptime_seconds;

/* Set up IDT entries, remap PIC (if used) and load IDTR. */
void idt_init(void);

/* Load IDTR from the current idtp descriptor. */
void idt_load(void);

#endif
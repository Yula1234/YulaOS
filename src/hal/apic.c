// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <drivers/vga.h>

#include "apic.h"
#include "io.h"

static uint32_t ticks_per_tick = 0;

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_ENABLE (1u << 11)
#define IA32_APIC_BASE_X2APIC (1u << 10)

static inline uint64_t rdmsr_u64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_u64(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void lapic_eoi() {
    lapic_write(LAPIC_EOI, 0);
}

static uint32_t calibrate_apic_timer() {
    uint32_t divisor = 1193182 / 100;
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)divisor);
    outb(0x40, (uint8_t)(divisor >> 8));

    lapic_write(LAPIC_TIMER_DIV, 0x3); 
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    uint16_t last_count = 0xFFFF;
    while (1) {
        outb(0x43, 0x00);
        uint8_t low = inb(0x40);
        uint8_t high = inb(0x40);
        uint16_t count = low | (high << 8);
        
        if (count > last_count) break;
        last_count = count;
    }

    lapic_write(LAPIC_TIMER, 0x10000);

    uint32_t ticks_passed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    return ticks_passed;
}

void lapic_init() {
    uint64_t apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);

    uint64_t new_base = apic_base;
    new_base |= IA32_APIC_BASE_ENABLE;
    new_base &= ~((uint64_t)IA32_APIC_BASE_X2APIC);
    if (new_base != apic_base) {
        wrmsr_u64(IA32_APIC_BASE_MSR, new_base);
    }

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x1FF);

    lapic_write(LAPIC_LINT0, 0x700);
    lapic_write(LAPIC_LINT1, 0x400);
    
    lapic_write(LAPIC_ERROR, 0);
    lapic_write(LAPIC_ERROR, 0);
    
    lapic_eoi();
}

void lapic_timer_init(uint32_t hz) {
    if (ticks_per_tick == 0) {
        uint32_t ticks_per_10ms = calibrate_apic_timer();
        
        if (ticks_per_10ms < 1000) {
            ticks_per_10ms = 150000;
        }
        
        uint32_t ticks_per_second = ticks_per_10ms * 100;
        ticks_per_tick = ticks_per_second / hz;
    }
    
    if (ticks_per_tick == 0) ticks_per_tick = 1000;

    lapic_write(LAPIC_TIMER, 32 | 0x20000); 
    lapic_write(LAPIC_TIMER_DIV, 0x3); 
    lapic_write(LAPIC_TIMER_INIT, ticks_per_tick);
}
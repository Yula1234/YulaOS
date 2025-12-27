#include <drivers/vga.h>

#include "apic.h"
#include "io.h"

static uint32_t ticks_per_tick = 0;

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
        outb(0x43, 0x00); // Latch command
        uint8_t low = inb(0x40);
        uint8_t high = inb(0x40);
        uint16_t count = low | (high << 8);
        
        if (count > last_count) break;
        last_count = count;
    }

    lapic_write(LAPIC_TIMER, 0x10000); // Masked

    uint32_t ticks_passed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    return ticks_passed;
}

void lapic_init() {
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
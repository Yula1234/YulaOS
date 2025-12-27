#include <drivers/vga.h>

#include "apic.h"
#include "io.h"

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

    uint32_t ticks_passed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    return ticks_passed;
}

void lapic_init() {
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0x1FF);

    lapic_write(LAPIC_LINT0, 0x700);
}

void lapic_timer_init(uint32_t hz) {
    uint32_t ticks_per_10ms = calibrate_apic_timer();
    
    uint32_t ticks_per_second = ticks_per_10ms * 100;
    uint32_t val = ticks_per_second / hz;

    lapic_write(LAPIC_TIMER, 32 | 0x20000); 
    lapic_write(LAPIC_TIMER_DIV, 0x3); 
    lapic_write(LAPIC_TIMER_INIT, val);
}
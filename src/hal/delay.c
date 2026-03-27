// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <hal/delay.h>

#include <hal/io.h>
#include <hal/irq.h>

uint64_t g_cpu_tsc_hz = 0u;

static inline uint64_t rdtsc_read(void) {
    uint32_t lo;
    uint32_t hi;

    __asm__ volatile(
        "rdtsc"
        : "=a"(lo), "=d"(hi)
    );

    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline uint16_t pit_read_counter0(void) {
    outb(0x43, 0x00u);

    const uint8_t lo = inb(0x40);
    const uint8_t hi = inb(0x40);

    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8u));
}

void hal_calibrate_tsc_hz(void) {
    if (g_cpu_tsc_hz != 0u) {
        return;
    }

    const uint32_t pit_input_hz = 1193182u;
    const uint32_t cal_ms = 10u;

    const uint32_t pit_div = (pit_input_hz * cal_ms) / 1000u;
    const uint16_t reload = (pit_div > 0xFFFFu) ? 0xFFFFu : (uint16_t)pit_div;

    const uint32_t eflags = get_eflags();

    __asm__ volatile("cli" ::: "memory");

    outb(0x43, 0x30u);
    outb(0x40, (uint8_t)(reload & 0xFFu));
    outb(0x40, (uint8_t)((reload >> 8u) & 0xFFu));

    const uint64_t t0 = rdtsc_read();

    for (;;) {
        if (pit_read_counter0() == 0u) {
            break;
        }

        __asm__ volatile("pause" ::: "memory");
    }

    const uint64_t t1 = rdtsc_read();

    if ((eflags & 0x200u) != 0u) {
        __asm__ volatile("sti" ::: "memory");
    }

    const uint64_t delta = (t1 > t0) ? (t1 - t0) : 0u;

    if (delta == 0u) {
        g_cpu_tsc_hz = 1000000000ull; 
        return;
    }

    g_cpu_tsc_hz = (delta * 1000ull) / (uint64_t)cal_ms;
}

void udelay(uint32_t us) {
    if (g_cpu_tsc_hz == 0u) {
        return;
    }

    if (us == 0u) {
        return;
    }

    const uint64_t start = rdtsc_read();
    const uint64_t wait_cycles = (g_cpu_tsc_hz * (uint64_t)us) / 1000000ull;

    while ((rdtsc_read() - start) < wait_cycles) {
        __asm__ volatile("pause" ::: "memory");
    }
}

void mdelay(uint32_t ms) {
    if (ms == 0u) {
        return;
    }

    udelay(ms * 1000u);
}
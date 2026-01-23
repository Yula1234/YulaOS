// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/rtc.h>

#include <hal/io.h>

static int rtc_is_updating(void) {
    outb(0x70, 0x0A);
    return (inb(0x71) & 0x80) != 0;
}

static uint8_t rtc_get_register(int reg) {
    outb(0x70, (uint8_t)reg);
    return inb(0x71);
}

void get_time_string(char* buf) {
    if (!buf) return;
    if (rtc_is_updating()) return;

    uint8_t s = rtc_get_register(0x00);
    uint8_t m = rtc_get_register(0x02);
    uint8_t h = rtc_get_register(0x04);

    s = (uint8_t)((s & 0x0F) + ((s / 16) * 10));
    m = (uint8_t)((m & 0x0F) + ((m / 16) * 10));
    h = (uint8_t)((h & 0x0F) + ((h / 16) * 10));

    h = (uint8_t)((h + 5) % 24);

    buf[0] = (char)((h / 10) + '0');
    buf[1] = (char)((h % 10) + '0');
    buf[2] = ':';
    buf[3] = (char)((m / 10) + '0');
    buf[4] = (char)((m % 10) + '0');
    buf[5] = ':';
    buf[6] = (char)((s / 10) + '0');
    buf[7] = (char)((s % 10) + '0');
    buf[8] = '\0';
}

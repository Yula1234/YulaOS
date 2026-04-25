/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <hal/cpu.h>
#include <hal/io.h>

#include <lib/compiler.h>

#include "ns16550.h"

#define UART_CLOCK_HZ 1843200u
#define DEFAULT_BAUD  115200u

#define REG_DATA 0u
#define REG_IER  1u
#define REG_IIR  2u
#define REG_FCR  2u
#define REG_LCR  3u
#define REG_MCR  4u
#define REG_LSR  5u
#define REG_MSR  6u
#define REG_SCR  7u

#define LSR_DATA_READY 0x01u
#define LSR_THR_EMPTY  0x20u

#define LCR_DLAB 0x80u
#define LCR_8N1  0x03u

#define MCR_DTR      0x01u
#define MCR_RTS      0x02u
#define MCR_OUT2     0x08u
#define MCR_LOOPBACK 0x10u

#define FCR_ENABLE_CLEAR_14B 0xC7u

___inline uint16_t get_reg_port(uint16_t base_port, uint16_t reg_offset) {
    return base_port + reg_offset;
}

___inline uint8_t read_reg(uint16_t base_port, uint16_t reg_offset) {
    const uint16_t port = get_reg_port(base_port, reg_offset);

    return inb(port);
}

___inline void write_reg(uint16_t base_port, uint16_t reg_offset, uint8_t value) {
    const uint16_t port = get_reg_port(base_port, reg_offset);

    outb(port, value);
}

static void set_baud_divisor(uint16_t port, uint16_t divisor) {
    const uint8_t lcr = read_reg(port, REG_LCR);

    write_reg(port, REG_LCR, (uint8_t)(lcr | LCR_DLAB));
    
    write_reg(port, REG_DATA, (uint8_t)(divisor & 0xFFu));
    write_reg(port, REG_IER,  (uint8_t)((divisor >> 8u) & 0xFFu));
    
    write_reg(port, REG_LCR, (uint8_t)(lcr & ~LCR_DLAB));
}

static int loopback_self_test(uint16_t port) {
    const uint8_t saved_mcr = read_reg(port, REG_MCR);

    write_reg(port, REG_MCR, MCR_LOOPBACK);

    write_reg(port, REG_DATA, 0xAEu);
    io_wait();

    const uint8_t got = read_reg(port, REG_DATA);

    write_reg(port, REG_MCR, saved_mcr);

    return (got == 0xAEu) ? 1 : 0;
}

int ns16550_can_read(uint16_t port) {
    const uint8_t lsr = read_reg(port, REG_LSR);

    if ((lsr & LSR_DATA_READY) != 0u) {
        return 1;
    }

    return 0;
}

int ns16550_can_write(uint16_t port) {
    const uint8_t lsr = read_reg(port, REG_LSR);

    if ((lsr & LSR_THR_EMPTY) != 0u) {
        return 1;
    }

    return 0;
}

void ns16550_init(uint16_t port) {
    write_reg(port, REG_IER, 0u);

    write_reg(port, REG_LCR, LCR_8N1);

    const uint16_t divisor = (uint16_t)(UART_CLOCK_HZ / (16u * DEFAULT_BAUD));
    set_baud_divisor(port, divisor);

    write_reg(port, REG_FCR, FCR_ENABLE_CLEAR_14B);

    write_reg(port, REG_MCR, (uint8_t)(MCR_DTR | MCR_RTS | MCR_OUT2));

    (void)loopback_self_test(port);

    (void)read_reg(port, REG_DATA);
    (void)read_reg(port, REG_LSR);
    (void)read_reg(port, REG_MSR);
}

void ns16550_putc(uint16_t port, char c) {
    while (!ns16550_can_write(port)) {
        cpu_relax();
    }

    if (c == '\n') {
        ns16550_putc(port, '\r');
    }

    write_reg(port, REG_DATA, (uint8_t)c);
}

char ns16550_getc(uint16_t port) {
    while (!ns16550_can_read(port)) {
        cpu_relax();
    }

    const uint8_t val = read_reg(port, REG_DATA);

    return (char)val;
}
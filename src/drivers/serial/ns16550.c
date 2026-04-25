/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/compiler.h>

#include <mm/heap.h>

#include "ns16550.h"

#define UART_CLOCK_HZ 1843200u

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

#define IER_BIT_RDA  0x01u
#define IER_BIT_THRE 0x02u

#define FCR_ENABLE_CLEAR_14B 0xC7u

#define IIR_INT_PENDING     0x01u
#define IIR_INT_STATUS_MASK 0x0Fu

#define IIR_INT_MODEM_STAT  0x00u
#define IIR_INT_TX_EMPTY    0x02u
#define IIR_INT_RX_DATA     0x04u
#define IIR_INT_LINE_STAT   0x06u
#define IIR_INT_RX_TIMEOUT  0x0Cu

typedef struct {
    uint8_t cached_ier;
} ns16550_state_t;

___inline uint8_t uart_read8(uart_port_t* port, uint16_t reg_offset) {
    return ioread8(port->iomem, reg_offset);
}

___inline void uart_write8(uart_port_t* port, uint16_t reg_offset, uint8_t value) {
    iowrite8(port->iomem, reg_offset, value);
}

___inline void uart_set_ier(uart_port_t* port, uint8_t value) {
    ns16550_state_t* state = (ns16550_state_t*)port->private_data;

    if (state->cached_ier == value) {
        return;
    }

    state->cached_ier = value;
    uart_write8(port, REG_IER, value);
}

static int ns16550_init(uart_port_t* port, uint32_t baud_rate) {
    if (unlikely(!port
        || !port->iomem)) {
        return -1;
    }

    ns16550_state_t* state = (ns16550_state_t*)kmalloc(sizeof(ns16550_state_t));
    if (!state) {
        return -1;
    }

    state->cached_ier = 0u;
    port->private_data = state;

    uart_write8(port, REG_IER, 0u);

    uart_write8(port, REG_LCR, LCR_8N1);

    const uint16_t divisor = (uint16_t)(UART_CLOCK_HZ / (16u * baud_rate));

    const uint8_t lcr = uart_read8(port, REG_LCR);

    uart_write8(port, REG_LCR, (uint8_t)(lcr | LCR_DLAB));
    
    uart_write8(port, REG_DATA, (uint8_t)(divisor & 0xFFu));
    uart_write8(port, REG_IER,  (uint8_t)((divisor >> 8u) & 0xFFu));
    
    uart_write8(port, REG_LCR, (uint8_t)(lcr & ~LCR_DLAB));

    uart_write8(port, REG_FCR, FCR_ENABLE_CLEAR_14B);

    uart_write8(port, REG_MCR, (uint8_t)(MCR_DTR | MCR_RTS | MCR_OUT2));

    (void)uart_read8(port, REG_DATA);
    (void)uart_read8(port, REG_LSR);
    (void)uart_read8(port, REG_MSR);

    uart_set_ier(port, IER_BIT_RDA);

    return 0;
}

static void ns16550_shutdown(uart_port_t* port) {
    if (unlikely(!port
        || !port->private_data)) {
        return;
    }

    uart_write8(port, REG_IER, 0u);

    kfree(port->private_data);
    
    port->private_data = NULL;
}

static void ns16550_tx_start(uart_port_t* port) {
    ns16550_state_t* state = (ns16550_state_t*)port->private_data;

    uart_set_ier(port, (uint8_t)(state->cached_ier | IER_BIT_THRE));
}

static void ns16550_tx_stop(uart_port_t* port) {
    ns16550_state_t* state = (ns16550_state_t*)port->private_data;

    uart_set_ier(port, (uint8_t)(state->cached_ier & ~IER_BIT_THRE));
}

static int ns16550_rx_ready(uart_port_t* port) {
    const uint8_t lsr = uart_read8(port, REG_LSR);

    return (lsr & LSR_DATA_READY) != 0u;
}

static int ns16550_tx_ready(uart_port_t* port) {
    const uint8_t lsr = uart_read8(port, REG_LSR);

    return (lsr & LSR_THR_EMPTY) != 0u;
}

static uint8_t ns16550_read_byte(uart_port_t* port) {
    return uart_read8(port, REG_DATA);
}

static void ns16550_write_byte(uart_port_t* port, uint8_t byte) {
    uart_write8(port, REG_DATA, byte);
}

static void ns16550_putc_sync(uart_port_t* port, char c) {
    while (!ns16550_tx_ready(port)) {}

    if (c == '\n') {
        ns16550_putc_sync(port, '\r');
    }

    uart_write8(port, REG_DATA, (uint8_t)c);
}

static void ns16550_handle_irq(uart_port_t* port) {
    for (;;) {
        const uint8_t iir = uart_read8(port, REG_IIR);

        if ((iir & IIR_INT_PENDING) != 0u) {
            break;
        }

        const uint8_t int_id = iir & IIR_INT_STATUS_MASK;

        switch (int_id) {
            case IIR_INT_LINE_STAT: {
                (void)uart_read8(port, REG_LSR);
                break;
            }

            case IIR_INT_RX_DATA:
            case IIR_INT_RX_TIMEOUT: {
                uart_core_on_rx_ready(port);
                break;
            }

            case IIR_INT_TX_EMPTY: {
                uart_core_on_tx_ready(port);
                break;
            }

            case IIR_INT_MODEM_STAT: {
                (void)uart_read8(port, REG_MSR);
                break;
            }

            default: {
                break;
            }
        }
    }
}


static const uart_ops_t g_ns16550_ops = {
    .init       = ns16550_init,
    .shutdown   = ns16550_shutdown,
    .tx_start   = ns16550_tx_start,
    .tx_stop    = ns16550_tx_stop,
    .rx_ready   = ns16550_rx_ready,
    .tx_ready   = ns16550_tx_ready,
    .read_byte  = ns16550_read_byte,
    .write_byte = ns16550_write_byte,
    .putc_sync  = ns16550_putc_sync,
    .handle_irq = ns16550_handle_irq,
};

const uart_ops_t* ns16550_get_ops(void) {
    return &g_ns16550_ops;
}
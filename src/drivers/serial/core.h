/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef DRIVERS_SERIAL_CORE_H
#define DRIVERS_SERIAL_CORE_H

#include <kernel/locking/spinlock.h>
#include <kernel/locking/sem.h>

#include <mm/iomem.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_RING_SIZE 4096u

typedef struct uart_port uart_port_t;

typedef struct uart_ops {
    int  (*init)(uart_port_t* port, uint32_t baud_rate);
    void (*shutdown)(uart_port_t* port);

    void (*tx_start)(uart_port_t* port);
    void (*tx_stop)(uart_port_t* port);

    int  (*rx_ready)(uart_port_t* port);
    int  (*tx_ready)(uart_port_t* port);

    uint8_t (*read_byte)(uart_port_t* port);
    void    (*write_byte)(uart_port_t* port, uint8_t byte);

    void (*putc_sync)(uart_port_t* port, char c);

    void (*handle_irq)(uart_port_t* port);
} uart_ops_t;

typedef struct uart_ring {
    uint8_t data[UART_RING_SIZE];

    size_t head;
    size_t tail;
    
    size_t count;
} uart_ring_t;

struct uart_port {
    const char* name;

    const uart_ops_t* ops;
    
    void* private_data;

    __iomem* iomem;

    uint8_t  irq_line;

    spinlock_t  tx_lock;
    uart_ring_t tx_ring;

    spinlock_t  rx_lock;
    uart_ring_t rx_ring;

    semaphore_t rx_sem;
};

int uart_port_register(uart_port_t* port, uint32_t baud_rate);
void uart_port_unregister(uart_port_t* port);

size_t uart_port_write(uart_port_t* port, const void* data, size_t size);
size_t uart_port_read(uart_port_t* port, void* data, size_t size);

void uart_port_wait_rx(uart_port_t* port);

void uart_port_poll(uart_port_t* port);

void uart_port_console_write(void* ctx, const char* data, size_t size);

void uart_core_on_rx_ready(uart_port_t* port);
void uart_core_on_tx_ready(uart_port_t* port);

#ifdef __cplusplus
}
#endif

#endif
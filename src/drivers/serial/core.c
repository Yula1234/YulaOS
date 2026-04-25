/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/locking/guards.h>
#include <kernel/panic.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <drivers/serial/core.h>

#include <hal/irq.h>

___inline size_t ring_free_space(const uart_ring_t* r) {
    return UART_RING_SIZE - r->count;
}

static int ring_push(uart_ring_t* r, uint8_t b) {
    if (unlikely(r->count >= UART_RING_SIZE)) {
        return 0;
    }

    r->data[r->head] = b;
    r->head++;

    if (unlikely(r->head >= UART_RING_SIZE)) {
        r->head = 0u;
    }

    r->count++;

    return 1;
}

static int ring_pop(uart_ring_t* r, uint8_t* out) {
    if (unlikely(r->count == 0u)) {
        return 0;
    }

    *out = r->data[r->tail];
    r->tail++;

    if (unlikely(r->tail >= UART_RING_SIZE)) {
        r->tail = 0u;
    }

    r->count--;

    return 1;
}

static size_t ring_peek_chunk(const uart_ring_t* r, const uint8_t** out_ptr) {
    if (unlikely(r->count == 0u)) {
        return 0u;
    }

    *out_ptr = &r->data[r->tail];

    const size_t contig = UART_RING_SIZE - r->tail;

    if (r->count < contig) {
        return r->count;
    }

    return contig;
}

static void ring_consume(uart_ring_t* r, size_t consumed) {
    if (unlikely(consumed == 0u)) {
        return;
    }

    r->tail = (r->tail + consumed) % UART_RING_SIZE;
    r->count -= consumed;
}

static void pump_rx_unlocked(uart_port_t* port) {
    int data_received = 0;

    while (port->ops->rx_ready(port)) {
        const uint8_t b = port->ops->read_byte(port);
        
        if (ring_push(&port->rx_ring, b)) {
            data_received = 1;
        }
    }

    if (data_received) {
        sem_signal_all(&port->rx_sem);
    }
}

static void pump_tx_unlocked(uart_port_t* port) {
    if (port->ops->write_buffer) {
        while (port->tx_ring.count != 0u) {
            const uint8_t* chunk_ptr = NULL;
            
            const size_t chunk_len = ring_peek_chunk(&port->tx_ring, &chunk_ptr);

            if (unlikely(chunk_len == 0u)) {
                break;
            }

            const size_t accepted = port->ops->write_buffer(port, chunk_ptr, chunk_len);

            if (accepted == 0u) {
                break;
            }

            ring_consume(&port->tx_ring, accepted);
        }
    } else {
        while (port->tx_ring.count != 0u) {
            if (!port->ops->tx_ready(port)) {
                break;
            }

            uint8_t b = 0u;

            if (!ring_pop(&port->tx_ring, &b)) {
                break;
            }

            port->ops->write_byte(port, b);
        }
    }

    if (port->tx_ring.count == 0u) {
        port->ops->tx_stop(port);
    } else {
        port->ops->tx_start(port);
    }
}

static void uart_irq_handler(___unused registers_t* regs, void* ctx) {
    uart_port_t* port = (uart_port_t*)ctx;

    if (unlikely(!port
        || !port->ops)) {
        return;
    }

    if (port->ops->handle_irq) {
        port->ops->handle_irq(port);
    }
}

void uart_core_on_rx_ready(uart_port_t* port) {
    if (unlikely(!port)) {
        return;
    }

    guard_spinlock_safe(&port->rx_lock);

    pump_rx_unlocked(port);
}

void uart_core_on_tx_ready(uart_port_t* port) {
    if (unlikely(!port)) {
        return;
    }

    guard_spinlock_safe(&port->tx_lock);

    pump_tx_unlocked(port);
}

int uart_port_register(uart_port_t* port, uint32_t baud_rate) {
    if (unlikely(!port
        || !port->ops)) {
        return -1;
    }

    spinlock_init(&port->tx_lock);
    spinlock_init(&port->rx_lock);

    sem_init(&port->rx_sem, 0);

    memset(&port->tx_ring, 0, sizeof(port->tx_ring));
    memset(&port->rx_ring, 0, sizeof(port->rx_ring));

    if (port->ops->init) {
        if (port->ops->init(port, baud_rate) != 0) {
            return -1;
        }
    }

    if (port->irq_line < 16u) {
        irq_install_handler(port->irq_line, uart_irq_handler, port);
    }

    return 0;
}

void uart_port_unregister(uart_port_t* port) {
    if (unlikely(!port)) {
        return;
    }

    if (port->ops && port->ops->shutdown) {
        port->ops->shutdown(port);
    }
}

size_t uart_port_write(uart_port_t* port, const void* data, size_t size) {
    if (unlikely(!port
        || !data
        || size == 0u)) {
        return 0u;
    }

    const uint8_t* bytes = (const uint8_t*)data;

    size_t written = 0u;

    guard_spinlock_safe(&port->tx_lock);

    while (written < size) {
        if (ring_free_space(&port->tx_ring) == 0u) {
            break;
        }

        if (!ring_push(&port->tx_ring, bytes[written])) {
            break;
        }

        written++;
    }

    if (written != 0u) {
        port->ops->tx_start(port);

        pump_tx_unlocked(port);
    }

    return written;
}

size_t uart_port_read(uart_port_t* port, void* data, size_t size) {
    if (unlikely(!port
        || !data
        || size == 0u)) {
        return 0u;
    }

    uint8_t* bytes = (uint8_t*)data;

    size_t read_bytes = 0u;

    guard_spinlock_safe(&port->rx_lock);

    while (read_bytes < size) {
        uint8_t b = 0u;

        if (!ring_pop(&port->rx_ring, &b)) {
            break;
        }

        bytes[read_bytes++] = b;
    }

    return read_bytes;
}

void uart_port_wait_rx(uart_port_t* port) {
    if (unlikely(!port)) {
        return;
    }

    sem_wait(&port->rx_sem);
}

void uart_port_poll(uart_port_t* port) {
    if (unlikely(!port)) {
        return;
    }

    {
        guard_spinlock_safe(&port->rx_lock);

        pump_rx_unlocked(port);
    }

    {
        guard_spinlock_safe(&port->tx_lock);

        pump_tx_unlocked(port);
    }
}

void uart_port_console_write(void* ctx, const char* data, size_t size) {
    uart_port_t* port = (uart_port_t*)ctx;

    if (unlikely(!port
        || !data
        || size == 0u)) {
        return;
    }


    if (unlikely(panic_in_progress())) {
        if (port->ops && port->ops->putc_sync) {
            for (size_t i = 0u; i < size; i++) {
                port->ops->putc_sync(port, data[i]);
            }
        }
        
        return;
    }

    (void)uart_port_write(port, data, size);
}
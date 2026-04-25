/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/serial/serial_core.h>
#include <drivers/serial/ns16550.h>

#include <kernel/locking/guards.h>

#include <hal/align.h>
#include <hal/irq.h>
#include <hal/io.h>

#include <lib/compiler.h>
#include <lib/string.h>

#define SERIAL_RING_SIZE 4096u

#define REG_DATA 0u
#define REG_IER  1u
#define REG_IIR  2u
#define REG_LSR  5u

#define IER_BIT_RDA  0x01u
#define IER_BIT_THRE 0x02u

#define LSR_BIT_DATA_READY 0x01u
#define LSR_BIT_THR_EMPTY  0x20u

typedef struct {
    uint8_t data[SERIAL_RING_SIZE];
    
    size_t  head;
    size_t  tail;
    size_t  count;
} serial_ring_t;

static uint16_t g_port = NS16550_COM1;

static __cacheline_aligned spinlock_t g_serial_core_lock;

static serial_ring_t g_rx;
static serial_ring_t g_tx;

static uint8_t g_cached_ier = 0u;

___inline size_t ring_free_space(const serial_ring_t* r) {
    return SERIAL_RING_SIZE - r->count;
}

static int ring_push(serial_ring_t* r, uint8_t b) {
    if (unlikely(r->count >= SERIAL_RING_SIZE)) {
        return 0;
    }

    r->data[r->head] = b;
    r->head++;

    if (unlikely(r->head >= SERIAL_RING_SIZE)) {
        r->head = 0u;
    }

    r->count++;

    return 1;
}

static int ring_pop(serial_ring_t* r, uint8_t* out) {
    if (unlikely(r->count == 0u)) {
        return 0;
    }

    *out = r->data[r->tail];
    r->tail++;

    if (unlikely(r->tail >= SERIAL_RING_SIZE)) {
        r->tail = 0u;
    }

    r->count--;

    return 1;
}


___inline uint16_t port_reg(uint16_t offset) {
    return g_port + offset;
}

___inline uint8_t uart_read8(uint16_t offset) {
    return inb(port_reg(offset));
}

___inline void uart_write8(uint16_t offset, uint8_t value) {
    outb(port_reg(offset), value);
}

___inline void uart_set_ier_cached(uint8_t value) {
    if (g_cached_ier == value) {
        return;
    }

    g_cached_ier = value;
    
    uart_write8(REG_IER, value);
}

___inline void uart_enable_thre_irq(void) {
    uart_set_ier_cached((uint8_t)(g_cached_ier | IER_BIT_THRE));
}

___inline void uart_disable_thre_irq(void) {
    uart_set_ier_cached((uint8_t)(g_cached_ier & ~IER_BIT_THRE));
}

static void pump_rx_unlocked(void) {
    while (ns16550_can_read(g_port)) {
        const char c = ns16550_getc(g_port);
        
        (void)ring_push(&g_rx, (uint8_t)c);
    }
}

static void pump_tx_irq_unlocked(void) {
    while (g_tx.count != 0u) {
        const uint8_t lsr = uart_read8(REG_LSR);
        
        if ((lsr & LSR_BIT_THR_EMPTY) == 0u) {
            break;
        }

        uint8_t b = 0u;
        
        if (!ring_pop(&g_tx, &b)) {
            break;
        }

        uart_write8(REG_DATA, b);
    }

    if (g_tx.count == 0u) {
        uart_disable_thre_irq();
    } else {
        uart_enable_thre_irq();
    }
}

static void serial_core_irq_handler(___unused registers_t* regs, ___unused void* ctx) {
    guard_spinlock_safe(&g_serial_core_lock);

    (void)uart_read8(REG_IIR);

    pump_rx_unlocked();

    pump_tx_irq_unlocked();
}

void serial_core_init(uint16_t port) {
    spinlock_init(&g_serial_core_lock);

    guard_spinlock_safe(&g_serial_core_lock);

    g_port = port;
    
    memset(&g_rx, 0, sizeof(g_rx));
    memset(&g_tx, 0, sizeof(g_tx));

    g_cached_ier = uart_read8(REG_IER);

    pump_rx_unlocked();

    uart_disable_thre_irq();

    irq_install_handler(4, serial_core_irq_handler, NULL);

    const uint8_t pic_mask = inb(0x21u);
    outb(0x21u, (uint8_t)(pic_mask & ~(1u << 4u)));
}

size_t serial_core_write(const void* data, size_t size) {
    if (unlikely(!data
        || size == 0u)) {
        return 0u;
    }

    const uint8_t* bytes = (const uint8_t*)data;

    size_t written = 0u;

    guard_spinlock_safe(&g_serial_core_lock);

    pump_rx_unlocked();

    while (written < size) {
        if (ring_free_space(&g_tx) == 0u) {
            break;
        }

        if (!ring_push(&g_tx, bytes[written])) {
            break;
        }

        written++;
    }

    if (written != 0u) {
        uart_enable_thre_irq();

        pump_tx_irq_unlocked();
    }

    return written;
}

size_t serial_core_read(void* data, size_t size) {
    if (unlikely(!data
        || size == 0u)) {
        return 0u;
    }

    uint8_t* bytes = (uint8_t*)data;
    size_t read_bytes = 0u;

    guard_spinlock_safe(&g_serial_core_lock);

    pump_rx_unlocked();

    while (read_bytes < size) {
        uint8_t b = 0u;
        
        if (!ring_pop(&g_rx, &b)) {
            break;
        }

        bytes[read_bytes++] = b;
    }

    return read_bytes;
}

size_t serial_core_rx_available(void) {
    guard_spinlock_safe(&g_serial_core_lock);

    pump_rx_unlocked();
    
    return g_rx.count;
}

size_t serial_core_tx_free(void) {
    guard_spinlock_safe(&g_serial_core_lock);

    return ring_free_space(&g_tx);
}

void serial_core_poll(void) {
    guard_spinlock_safe(&g_serial_core_lock);

    pump_rx_unlocked();
    pump_tx_irq_unlocked();
}

void serial_core_console_write(___unused void* ctx, const char* data, size_t size) {
    (void)serial_core_write(data, size);
}
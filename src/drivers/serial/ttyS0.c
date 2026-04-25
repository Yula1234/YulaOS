/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/output/console.h>
#include <kernel/tty/core.h>
#include <kernel/proc.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <hal/pmio.h>
#include <hal/pic.h>

#include <drivers/driver.h>

#include <mm/heap.h>

#include "ns16550.h"
#include "core.h"

static uart_port_t g_com1_port;

static tty_t* g_ttyS0 = NULL;

static vfs_node_t g_ttyS0_node;

static int ttyS0_hw_write(___unused tty_t* tty, const void* buf, uint32_t size) {
    if (!buf
        || size == 0u) {
        return 0;
    }

    const size_t written = uart_port_write(&g_com1_port, buf, size);

    return (int)written;
}

static int ttyS0_hw_poll_status(___unused tty_t* tty, int events) {
    int revents = 0;

    if ((events & VFS_POLLOUT) != 0) {
        revents |= VFS_POLLOUT;
    }

    return revents;
}

static const tty_driver_ops_t g_ttyS0_ops = {
    .write         = ttyS0_hw_write,
    .poll_status   = ttyS0_hw_poll_status,

    .open          = NULL, 
    .ioctl         = NULL,
    .close         = NULL, 
    .set_termios   = NULL,
};

static void ttyS0_rx_kthread(void* arg) {
    (void)arg;

    uint8_t rx_buf[128];

    for (;;) {
        uart_port_wait_rx(&g_com1_port);

        const size_t n = uart_port_read(&g_com1_port, rx_buf, sizeof(rx_buf));

        if (n > 0u) {
            tty_receive(g_ttyS0, rx_buf, (uint32_t)n);
        }
    }
}


void __do_init_uart(void) {
    memset(&g_com1_port, 0, sizeof(g_com1_port));

    g_com1_port.name     = "COM1";
    g_com1_port.ops      = ns16550_get_ops();
    g_com1_port.irq_line = 4u;
    
    g_com1_port.iomem    = iomem_request_pmio(NS16550_COM1_BASE, 8u, "com1");

    if (uart_port_register(&g_com1_port, 115200u) != 0) {
        iomem_free(g_com1_port.iomem);
    }

    (void)pic_unmask_irq(g_com1_port.irq_line);

    console_set_writer(uart_port_console_write, &g_com1_port);

    g_ttyS0 = tty_alloc(&g_ttyS0_ops, NULL);

    if (!g_ttyS0) {
        uart_port_unregister(&g_com1_port);
    }

    memset(&g_ttyS0_node, 0, sizeof(g_ttyS0_node));
    
    strlcpy(g_ttyS0_node.name, "ttyS0", sizeof(g_ttyS0_node.name));

    g_ttyS0_node.refs = 1u;

    tty_bind_vfs_node(g_ttyS0, &g_ttyS0_node);

    ___unused task_t* rx_worker = proc_spawn_kthread(
        "ttyS0_rx", PRIO_HIGH,
        ttyS0_rx_kthread, NULL
    );
}

static int ttyS0_driver_init(void) {
    devfs_register(&g_ttyS0_node);

    return 0;
}

DRIVER_REGISTER(
    .name     = "ttyS0",
    .klass    = DRIVER_CLASS_CHAR,
    .stage    = DRIVER_STAGE_VFS,
    .init     = ttyS0_driver_init,
    .shutdown = NULL
);
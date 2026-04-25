/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/locking/guards.h>
#include <kernel/tty/core.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <drivers/serial/serial_core.h>
#include <drivers/driver.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <mm/heap.h>

#include <fs/vfs.h>

static tty_t* g_ttyS0 = NULL;

static vfs_node_t g_ttyS0_node;

static int ttyS0_hw_write(___unused tty_t* tty, const void* buf, uint32_t size) {
    if (unlikely(!buf
        || size == 0u)) {
        return 0;
    }

    const uint8_t* data = (const uint8_t*)buf;

    const size_t written = serial_core_write(data, size);

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

    .open = NULL, .ioctl = NULL,
    .close = NULL, .set_termios = NULL,
};

static void ttyS0_rx_kthread(___unused void* arg) {
    uint8_t rx_buf[64];

    for (;;) {
        serial_core_poll();

        const size_t n = serial_core_read(rx_buf, sizeof(rx_buf));

        if (likely(n > 0)) {
            tty_receive(g_ttyS0, rx_buf, (uint32_t)n);
        } else {
            proc_usleep(2000);
        }
    }
}

static int ttyS0_driver_init(void) {
    g_ttyS0 = tty_alloc(&g_ttyS0_ops, NULL);

    if (unlikely(!g_ttyS0)) {
        return -1;
    }

    memset(&g_ttyS0_node, 0, sizeof(g_ttyS0_node));
    
    strlcpy(g_ttyS0_node.name, "ttyS0", sizeof(g_ttyS0_node.name));

    g_ttyS0_node.refs = 1u;

    tty_bind_vfs_node(g_ttyS0, &g_ttyS0_node);

    devfs_register(&g_ttyS0_node);

    ___unused task_t* rx_worker = proc_spawn_kthread(
        "ttyS0_rx", PRIO_HIGH,
        ttyS0_rx_kthread, NULL
    );

    return 0;
}

DRIVER_REGISTER(
    .name = "ttyS0",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = ttyS0_driver_init,
    .shutdown = NULL
);
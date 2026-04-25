/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/tty/tty_service.h>
#include <kernel/tty/tty_internal.h>
#include <kernel/tty/core.h>

#include <kernel/term/term.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <drivers/serial/serial_core.h>
#include <drivers/serial/ttyS0.h>

#include <lib/cpp/lock_guard.h>
#include <lib/compiler.h>
#include <lib/string.h>

#include <mm/heap.h>

#include <fs/vfs.h>

namespace {

static tty_t* g_ttyS0 = nullptr;

static vfs_node_t g_ttyS0_node;

static kernel::term::Term* get_active_term(void) {
    tty_handle_t* active = kernel::tty::TtyService::instance().get_active_for_render();

    return tty_term_ptr(active);
}

static int ttyS0_hw_write(___unused tty_t* tty, const void* buf, uint32_t size) {
    if (!buf || size == 0u) {
        return 0;
    }

    const uint8_t* data = static_cast<const uint8_t*>(buf);

    size_t written = serial_core_write(data, size);

    kernel::term::Term* term = get_active_term();

    if (term) {
        term->write(static_cast<const char*>(buf), size);

        kernel::tty::TtyService::instance().request_render(
            kernel::tty::TtyService::RenderReason::Output
        );
    }

    return static_cast<int>(written);
}

static int ttyS0_hw_poll_status(___unused tty_t* tty, int events) {
    int revents = 0;

    if ((events & VFS_POLLOUT) != 0) {
        revents |= VFS_POLLOUT;
    }

    return revents;
}

static const tty_driver_ops_t g_ttyS0_ops = {
    .open          = nullptr,
    .write         = ttyS0_hw_write,
    .ioctl         = nullptr,
    .close         = nullptr,
    .set_termios   = nullptr,
    .poll_status   = ttyS0_hw_poll_status,
};

static void ttyS0_rx_kthread(void* arg) {
    (void)arg;

    uint8_t rx_buf[64];

    for (;;) {
        serial_core_poll();

        size_t n = serial_core_read(rx_buf, sizeof(rx_buf));

        if (n > 0) {
            tty_receive(g_ttyS0, rx_buf, static_cast<uint32_t>(n));
        } else {
            proc_usleep(2000);
        }
    }
}

}

extern "C" void ttyS0_init(void) {
    g_ttyS0 = tty_alloc(&g_ttyS0_ops, nullptr);

    if (!g_ttyS0) {
        return;
    }

    memset(&g_ttyS0_node, 0, sizeof(g_ttyS0_node));
    
    strlcpy(g_ttyS0_node.name, "ttyS0", sizeof(g_ttyS0_node.name));

    g_ttyS0_node.refs = 1u;

    tty_bind_vfs_node(g_ttyS0, &g_ttyS0_node);

    devfs_register(&g_ttyS0_node);

    ___unused task_t* rx_worker = proc_spawn_kthread(
        "ttyS0_rx", PRIO_HIGH,
        ttyS0_rx_kthread,
        nullptr
    );
}
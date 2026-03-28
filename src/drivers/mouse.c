// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/idt.h>

#include <fs/vfs.h>
#include <drivers/mouse.h>
#include <drivers/fbdev.h>
#include <drivers/virtio/vgpu.h>

#include <kernel/input_focus.h>
#include <kernel/proc.h>
#include <kernel/waitq/poll_waitq.h>

#include <hal/irq.h>
#include <hal/lock.h>
#include <hal/pmio.h>
#include <hal/pic.h>

#include "mouse.h"

int mouse_x = 512, mouse_y = 384;
int mouse_buttons = 0;

static poll_waitq_t mouse_poll_waitq;

static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[3];

enum {
    PS2_PORT_DATA = 0x60u,
    PS2_PORT_STATUS = 0x64u,
    PS2_PORT_COMMAND = 0x64u,
};

static pmio_region_t* g_ps2_data_region = 0;
static pmio_region_t* g_ps2_cmd_region = 0;

static __cacheline_aligned spinlock_t g_ps2_lock;
static int g_ps2_pmio_inited = 0;

static int mouse_pmio_is_ready(void) {
    return g_ps2_data_region && g_ps2_cmd_region;
}

static int mouse_pmio_init(void) {
    if (g_ps2_pmio_inited) {
        return 0;
    }

    spinlock_init(&g_ps2_lock);

    g_ps2_data_region = pmio_request_region(PS2_PORT_DATA, 1u, "ps2_data");
    if (!g_ps2_data_region) {
        return -1;
    }

    g_ps2_cmd_region = pmio_request_region(PS2_PORT_COMMAND, 1u, "ps2_ctrl");
    if (!g_ps2_cmd_region) {
        pmio_release_region(g_ps2_data_region);
        g_ps2_data_region = 0;

        return -1;
    }

    g_ps2_pmio_inited = 1;

    return 0;
}

static uint8_t mouse_ps2_read_status(void) {
    if (!mouse_pmio_is_ready()) {
        return 0u;
    }

    uint8_t status = 0u;
    (void)pmio_readb(g_ps2_cmd_region, 0u, &status);
    return status;
}

static void mouse_ps2_write_command(uint8_t cmd) {
    if (!mouse_pmio_is_ready()) {
        return;
    }

    (void)pmio_writeb(g_ps2_cmd_region, 0u, cmd);
}

static void mouse_ps2_write_data(uint8_t data) {
    if (!mouse_pmio_is_ready()) {
        return;
    }

    (void)pmio_writeb(g_ps2_data_region, 0u, data);
}

static uint8_t mouse_ps2_read_data(void) {
    if (!mouse_pmio_is_ready()) {
        return 0u;
    }

    uint8_t data = 0u;
    (void)pmio_readb(g_ps2_data_region, 0u, &data);
    return data;
}

static int mouse_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer) return -1;
    if (size < sizeof(mouse_state_t)) return -1;

    task_t* curr = proc_current();
    uint32_t owner_pid = fb_get_owner_pid();
    if (owner_pid != 0) {
        if (!curr || curr->pid != owner_pid) {
            return 0;
        }
    } else {
        uint32_t focus_pid = input_focus_get_pid();
        if (focus_pid > 0 && curr && curr->pid != focus_pid) {
            return 0;
        }
    }

    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    mouse_state_t st;
    st.x = mouse_x;
    st.y = mouse_y;
    st.buttons = mouse_buttons;

    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory");

    *(mouse_state_t*)buffer = st;
    return (int)sizeof(mouse_state_t);
}

int mouse_poll_ready(task_t* task) {
    if (!task) return 0;
    uint32_t owner_pid = fb_get_owner_pid();
    if (owner_pid != 0) {
        return task->pid == owner_pid;
    }
    uint32_t focus_pid = input_focus_get_pid();
    if (focus_pid > 0 && task->pid != focus_pid) {
        return 0;
    }
    return 1;
}

int mouse_poll_waitq_register(poll_waiter_t* w, task_t* task) {
    if (!w || !task) return -1;
    return poll_waitq_register(&mouse_poll_waitq, w, task);
}

static int mouse_vfs_poll_status(vfs_node_t* node, int events) {
    (void)node;

    if ((events & VFS_POLLIN) == 0) {
        return 0;
    }

    task_t* curr = proc_current();
    if (!curr) {
        return 0;
    }

    if (mouse_poll_ready(curr)) {
        return VFS_POLLIN;
    }

    return 0;
}

static int mouse_vfs_poll_register(vfs_node_t* node, poll_waiter_t* w, task_t* task) {
    (void)node;
    return mouse_poll_waitq_register(w, task);
}

void mouse_poll_notify_focus_change(void) {
    poll_waitq_wake_all(&mouse_poll_waitq);
}

static vfs_ops_t mouse_ops = {
    .read = mouse_vfs_read,
    .write = 0,
    .open = 0,
    .close = 0,
    .ioctl = 0,
    .get_phys_page = 0,
    .poll_status = mouse_vfs_poll_status,
    .poll_register = mouse_vfs_poll_register,
};

static vfs_node_t mouse_node = { .name = "mouse", .ops = &mouse_ops, .size = sizeof(mouse_state_t) };

void mouse_inject_delta(int dx, int dy, int buttons) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    mouse_buttons = buttons & 0x07;

    mouse_x += dx;
    mouse_y += dy;

    int max_w = (int)fb_width;
    int max_h = (int)fb_height;
    if (virtio_gpu_is_active()) {
        const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();
        if (fb && fb->width > 0u && fb->height > 0u) {
            max_w = (int)fb->width;
            max_h = (int)fb->height;
        }
    }
    if (max_w < 1) max_w = 1;
    if (max_h < 1) max_h = 1;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= max_w) mouse_x = max_w - 1;
    if (mouse_y >= max_h) mouse_y = max_h - 1;

    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory");

    poll_waitq_wake_all(&mouse_poll_waitq);
}

void mouse_vfs_init(void) {
    poll_waitq_init(&mouse_poll_waitq);
    devfs_register(&mouse_node);
}

static void mouse_wait_unlocked(uint8_t type) {
    uint32_t timeout = 100000;

    if (!mouse_pmio_is_ready()) {
        return;
    }

    if (type == 0) {
        while (timeout--) {
            if ((mouse_ps2_read_status() & 1u) == 1u) {
                return;
            }
        }
    } else {
        while (timeout--) {
            if ((mouse_ps2_read_status() & 2u) == 0u) {
                return;
            }
        }
    }
}

static void mouse_write_unlocked(uint8_t a) {
    mouse_wait_unlocked(1);
    mouse_ps2_write_command(0xD4u);
    mouse_wait_unlocked(1);
    mouse_ps2_write_data(a);
}

static uint8_t mouse_read_unlocked(void) {
    mouse_wait_unlocked(0);
    return mouse_ps2_read_data();
}

void mouse_wait(uint8_t type) {
    if (!mouse_pmio_is_ready()) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&g_ps2_lock);

    mouse_wait_unlocked(type);

    spinlock_release_safe(&g_ps2_lock, flags);
}

void mouse_write(uint8_t a) {
    if (!mouse_pmio_is_ready()) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&g_ps2_lock);

    mouse_write_unlocked(a);

    spinlock_release_safe(&g_ps2_lock, flags);
}

uint8_t mouse_read() {
    if (!mouse_pmio_is_ready()) {
        return 0u;
    }

    const uint32_t flags = spinlock_acquire_safe(&g_ps2_lock);

    const uint8_t data = mouse_read_unlocked();

    spinlock_release_safe(&g_ps2_lock, flags);

    return data;
}

void mouse_irq_handler(registers_t* regs) {
    (void)regs;

    if (!mouse_pmio_is_ready()) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&g_ps2_lock);

    uint8_t status = mouse_ps2_read_status();
    
    if (!(status & 0x01u)) {
        spinlock_release_safe(&g_ps2_lock, flags);
        return;
    }

    uint8_t data = mouse_ps2_read_data();

    spinlock_release_safe(&g_ps2_lock, flags);

    if (status & 0x20) {
        mouse_process_byte(data);
    }
}

static void mouse_irq_handler_trampoline(registers_t* regs, void* ctx) {
    (void)ctx;
    mouse_irq_handler(regs);
}

void mouse_init() {
    if (mouse_pmio_init() != 0) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&g_ps2_lock);

    uint8_t status;

    mouse_wait_unlocked(1);
    mouse_ps2_write_command(0xA8u);

    mouse_wait_unlocked(1);
    mouse_ps2_write_command(0x20u);
    status = mouse_read_unlocked() | 2;
    status &= ~0x20;          

    mouse_wait_unlocked(1);
    mouse_ps2_write_command(0x60u);
    mouse_wait_unlocked(1);
    mouse_ps2_write_data(status);

    mouse_write_unlocked(0xF6);
    (void)mouse_read_unlocked();

    mouse_write_unlocked(0xF4);
    (void)mouse_read_unlocked();

    irq_install_handler(12, mouse_irq_handler_trampoline, 0);

    (void)pic_unmask_irq(12u);

    spinlock_release_safe(&g_ps2_lock, flags);
}

void mouse_process_byte(uint8_t data) {
    if (mouse_cycle == 0 && !(data & 0x08)) return;

    mouse_byte[mouse_cycle++] = data;

    if (mouse_cycle == 3) {
        mouse_cycle = 0;
        
        if (mouse_byte[0] & 0x80 || mouse_byte[0] & 0x40) return;

        int32_t rel_x = (int8_t)mouse_byte[1];
        int32_t rel_y = (int8_t)mouse_byte[2];

        mouse_buttons = mouse_byte[0] & 0x07;

        mouse_x += rel_x;
        mouse_y -= rel_y;

        int max_w = (int)fb_width;
        int max_h = (int)fb_height;
        if (virtio_gpu_is_active()) {
            const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();
            if (fb && fb->width > 0u && fb->height > 0u) {
                max_w = (int)fb->width;
                max_h = (int)fb->height;
            }
        }
        if (max_w < 1) max_w = 1;
        if (max_h < 1) max_h = 1;

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= max_w) mouse_x = max_w - 1;
        if (mouse_y >= max_h) mouse_y = max_h - 1;

        poll_waitq_wake_all(&mouse_poll_waitq);
    }
}

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <fs/vfs.h>
#include <hal/lock.h>
#include <kernel/input_focus.h>

#include <kernel/tty/tty_bridge.h>

#include "fbdev.h"

#include <drivers/virtio_gpu.h>

uint32_t* fb_ptr = 0;
uint32_t  fb_width = 0;
uint32_t  fb_height = 0;
uint32_t  fb_pitch = 0;
volatile int g_fb_mapped = 0;

static spinlock_t fb_owner_lock;
static uint32_t fb_owner_pid;
static int fb_prev_focus_pid;

static int fb0_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;

    if (!buffer) return -1;
    if (offset != 0) return 0;
    if (size < sizeof(fb_info_t)) return -1;

    fb_info_t info;

    if (virtio_gpu_is_active()) {
        const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();
        if (!fb) return -1;
        info.width = fb->width;
        info.height = fb->height;
        info.pitch = fb->pitch;
    } else {
        info.width = fb_width;
        info.height = fb_height;
        info.pitch = fb_pitch;
    }

    info.stride = info.width * 4u;
    info.bpp = 32;
    info.size_bytes = info.pitch * info.height;

    *(fb_info_t*)buffer = info;
    return (int)sizeof(fb_info_t);
}

static vfs_ops_t fb0_ops = { .read = fb0_vfs_read };
static vfs_node_t fb0_node = { .name = "fb0", .ops = &fb0_ops, .size = sizeof(fb_info_t) };

uint32_t fb_get_owner_pid(void) {
    uint32_t flags = spinlock_acquire_safe(&fb_owner_lock);
    uint32_t pid = fb_owner_pid;
    spinlock_release_safe(&fb_owner_lock, flags);
    return pid;
}

int fb_acquire(uint32_t pid) {
    if (pid == 0) return -1;
    uint32_t flags = spinlock_acquire_safe(&fb_owner_lock);
    int ok = 0;
    if (fb_owner_pid == 0 || fb_owner_pid == pid) {
        if (fb_owner_pid == 0) {
            uint32_t prev = input_focus_exchange_pid(pid);
            fb_prev_focus_pid = (prev == pid) ? 0 : (int)prev;
        } else {
            input_focus_set_pid(pid);
        }
        fb_owner_pid = pid;
        ok = 1;
    }
    spinlock_release_safe(&fb_owner_lock, flags);
    return ok ? 0 : -1;
}

int fb_release(uint32_t pid) {
    if (pid == 0) return -1;
    uint32_t flags = spinlock_acquire_safe(&fb_owner_lock);
    int ok = 0;
    if (fb_owner_pid == pid) {
        fb_owner_pid = 0;
        if (input_focus_get_pid() == pid) {
            input_focus_set_pid((uint32_t)fb_prev_focus_pid);
        }
        fb_prev_focus_pid = 0;
        ok = 1;
    }
    spinlock_release_safe(&fb_owner_lock, flags);

    if (ok) {
        tty_bridge_force_redraw_active();
    }

    return ok ? 0 : -1;
}

void fb_release_by_pid(uint32_t pid) {
    if (pid == 0) return;
    uint32_t flags = spinlock_acquire_safe(&fb_owner_lock);
    int ok = 0;
    if (fb_owner_pid == pid) {
        fb_owner_pid = 0;
        if (input_focus_get_pid() == pid) {
            input_focus_set_pid((uint32_t)fb_prev_focus_pid);
        }
        fb_prev_focus_pid = 0;
        ok = 1;
    }
    spinlock_release_safe(&fb_owner_lock, flags);

    if (ok) {
        tty_bridge_force_redraw_active();
    }
}

int fb_kernel_can_render(void) {
    uint32_t flags = spinlock_acquire_safe(&fb_owner_lock);
    int ok = (fb_owner_pid == 0);
    spinlock_release_safe(&fb_owner_lock, flags);
    return ok;
}

void fb_vfs_init(void) {
    spinlock_init(&fb_owner_lock);
    fb_owner_pid = 0;
    fb_prev_focus_pid = 0;
    devfs_register(&fb0_node);
}

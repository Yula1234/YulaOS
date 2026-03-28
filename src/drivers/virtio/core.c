// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <drivers/virtio/core.h>

#include <hal/lock.h>

static dlist_head_t g_virtio_drivers;
static spinlock_t g_virtio_drivers_lock;
static volatile int g_virtio_drivers_subsystem_inited;

static void virtio_drivers_subsystem_init(void) {
    if (__sync_bool_compare_and_swap(&g_virtio_drivers_subsystem_inited, 0, 1)) {
        spinlock_init(&g_virtio_drivers_lock);
        dlist_init(&g_virtio_drivers);
    }
}

int virtio_register_driver(virtio_driver_t* driver) {
    if (!driver || !driver->probe) {
        return -1;
    }

    virtio_drivers_subsystem_init();

    if (dlist_node_linked(&driver->drivers_node)) {
        return -1;
    }

    const uint32_t iflags = spinlock_acquire_safe(&g_virtio_drivers_lock);

    dlist_add_tail(&driver->drivers_node, &g_virtio_drivers);

    spinlock_release_safe(&g_virtio_drivers_lock, iflags);

    return 0;
}

int virtio_register_device(virtio_device_t* vdev) {
    if (!vdev || !vdev->ops) {
        return -1;
    }

    virtio_drivers_subsystem_init();

    const uint32_t iflags = spinlock_acquire_safe(&g_virtio_drivers_lock);

    virtio_driver_t* matched = 0;
    virtio_driver_t* pos = 0;

    dlist_for_each_entry(pos, &g_virtio_drivers, drivers_node) {
        if (pos->device_type == vdev->virtio_dev_id) {
            matched = pos;
            break;
        }
    }

    spinlock_release_safe(&g_virtio_drivers_lock, iflags);

    if (!matched) {
        return -1;
    }

    const int rc = matched->probe(vdev);
    if (rc == 0) {
        vdev->attached_virtio_driver = matched;
    }

    return rc;
}

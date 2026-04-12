/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <drivers/block/bdev.h>

#include <mm/heap.h>

#include <lib/string.h>
#include <lib/dlist.h>
#include <lib/compiler.h>

#include <hal/lock.h>


typedef struct {
    spinlock_t lock_;
    dlist_head_t device_list_;
} BdevRegistry;


static BdevRegistry g_bdev;

static block_device_t* g_bdev_root = 0;


static void bdev_init_node_template(block_device_t* dev);

static int bdev_vfs_open(vfs_node_t* node);

static int bdev_vfs_close(vfs_node_t* node);

static int bdev_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer);

static int bdev_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer);


static void bdev_node_private_retain(void* private_data) {
    bdev_retain((block_device_t*)private_data);
}

static void bdev_node_private_release(void* private_data) {
    bdev_release((block_device_t*)private_data);
}


void bdev_retain(block_device_t* dev) {
    if (unlikely(!dev)) {
        return;
    }

    if (dev->private_retain && dev->private_data) {
        dev->private_retain(dev->private_data);
    }

    atomic_uint_fetch_add_explicit(&dev->refs_, 1u, ATOMIC_RELAXED);
}

void bdev_release(block_device_t* dev) {
    if (unlikely(!dev)) {
        return;
    }

    if (dev->private_release && dev->private_data) {
        dev->private_release(dev->private_data);
    }

    const uint32_t old_refs = atomic_uint_fetch_sub_explicit(&dev->refs_, 1u, ATOMIC_ACQ_REL);

    if (old_refs == 1u) {
        if (dev->destroy) {
            dev->destroy(dev);
            return;
        }

        kfree(dev);
    }
}

void bdev_init(void) {
    spinlock_init(&g_bdev.lock_);
    dlist_init(&g_bdev.device_list_);

    g_bdev_root = 0;
}

void bdev_set_root(block_device_t* dev) {
    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock_);

    g_bdev_root = dev;

    spinlock_release_safe(&g_bdev.lock_, flags);
}

block_device_t* bdev_root(void) {
    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock_);

    block_device_t* out = g_bdev_root;

    spinlock_release_safe(&g_bdev.lock_, flags);

    return out;
}

static block_device_t* bdev_find_locked(const char* name) {
    if (!name) {
        return 0;
    }

    block_device_t* dev;

    dlist_for_each_entry(dev, &g_bdev.device_list_, list_node) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
    }

    return 0;
}

int bdev_register(block_device_t* dev) {
    if (!dev
        || !dev->ops
        || !dev->ops->read_sectors
        || !dev->name) {
        return -1;
    }

    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock_);

    if (bdev_find_locked(dev->name)) {
        spinlock_release_safe(&g_bdev.lock_, flags);
        return -1;
    }

    atomic_uint_store_explicit(&dev->refs_, 1u, ATOMIC_RELAXED);

    if (dev->private_retain && dev->private_data) {
        dev->private_retain(dev->private_data);
    }

    bdev_init_node_template(dev);

    dlist_add_tail(&dev->list_node, &g_bdev.device_list_);

    spinlock_release_safe(&g_bdev.lock_, flags);

    devfs_register(&dev->node_template);

    return 0;
}

int bdev_unregister(const char* name) {
    if (!name) {
        return -1;
    }

    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock_);

    block_device_t* dev = bdev_find_locked(name);

    if (!dev) {
        spinlock_release_safe(&g_bdev.lock_, flags);
        return -1;
    }

    dlist_del(&dev->list_node);

    spinlock_release_safe(&g_bdev.lock_, flags);

    (void)devfs_unregister(name);

    bdev_release(dev);

    return 0;
}

block_device_t* bdev_find_by_name(const char* name) {
    if (!name) {
        return 0;
    }

    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock_);

    block_device_t* dev = bdev_find_locked(name);

    if (dev) {
        bdev_retain(dev);
    }

    spinlock_release_safe(&g_bdev.lock_, flags);

    return dev;
}

block_device_t* bdev_first(void) {
    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock_);

    block_device_t* out = 0;

    if (!dlist_empty(&g_bdev.device_list_)) {
        out = container_of(g_bdev.device_list_.next, block_device_t, list_node);

        bdev_retain(out);
    }

    spinlock_release_safe(&g_bdev.lock_, flags);

    return out;
}

static int bdev_check_bounds(const block_device_t* dev, uint64_t lba, uint32_t count) {
    if (!dev) {
        return 0;
    }

    if (count == 0) {
        return 1;
    }

    const uint64_t end = lba + (uint64_t)count;

    if (end < lba
        || end > dev->sector_count) {
        return 0;
    }

    return 1;
}

int bdev_read_sectors(block_device_t* dev, uint64_t lba, uint32_t count, void* buf) {
    if (!dev
        || !dev->ops
        || !dev->ops->read_sectors
        || !buf) {
        return 0;
    }

    if (!bdev_check_bounds(dev, lba, count)) {
        return 0;
    }

    const int result = dev->ops->read_sectors(dev, lba, count, buf);

    return result != 0;
}

int bdev_write_sectors(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf) {
    if (!dev
        || !dev->ops
        || !dev->ops->write_sectors
        || !buf) {
        return 0;
    }

    if (!bdev_check_bounds(dev, lba, count)) {
        return 0;
    }

    const int result = dev->ops->write_sectors(dev, lba, count, buf);

    return result != 0;
}

int bdev_flush(block_device_t* dev) {
    if (!dev
        || !dev->ops
        || !dev->ops->flush) {
        return 1;
    }

    const int result = dev->ops->flush(dev);

    return result != 0;
}

static vfs_ops_t g_bdev_vfs_ops = {
    .read = bdev_vfs_read,
    .write = bdev_vfs_write,
    .open = bdev_vfs_open,
    .close = bdev_vfs_close,
};

static void bdev_init_node_template(block_device_t* dev) {
    if (!dev) {
        return;
    }

    memset(&dev->node_template, 0, sizeof(dev->node_template));

    if (dev->name) {
        strlcpy(dev->node_template.name, dev->name, sizeof(dev->node_template.name));
    }

    dev->node_template.ops = &g_bdev_vfs_ops;
    dev->node_template.private_data = dev;

    dev->node_template.private_retain = bdev_node_private_retain;
    dev->node_template.private_release = bdev_node_private_release;
}

static int bdev_vfs_open(vfs_node_t* node) {
    if (!node || !node->private_data) {
        return -1;
    }

    return 0;
}

static int bdev_vfs_close(vfs_node_t* node) {
    if (!node || !node->private_data) {
        return -1;
    }

    return 0;
}

static int bdev_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    if (!node
        || !buffer) {
        return -1;
    }

    block_device_t* dev = (block_device_t*)node->private_data;

    if (!dev) {
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    const uint32_t sector_size = dev->sector_size;

    if (sector_size == 0
        || (offset % sector_size) != 0
        || (size % sector_size) != 0) {
        return -1;
    }

    const uint64_t lba = (uint64_t)offset / (uint64_t)sector_size;
    const uint32_t count = size / sector_size;

    if (!bdev_read_sectors(dev, lba, count, buffer)) {
        return -1;
    }

    return (int)size;
}

static int bdev_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    if (!node
        || !buffer) {
        return -1;
    }

    block_device_t* dev = (block_device_t*)node->private_data;

    if (!dev) {
        return -1;
    }

    if (size == 0) {
        return 0;
    }

    const uint32_t sector_size = dev->sector_size;

    if (sector_size == 0
        || (offset % sector_size) != 0
        || (size % sector_size) != 0) {
        return -1;
    }

    const uint64_t lba = (uint64_t)offset / (uint64_t)sector_size;
    const uint32_t count = size / sector_size;

    if (!bdev_write_sectors(dev, lba, count, buffer)) {
        return -1;
    }

    return (int)size;
}
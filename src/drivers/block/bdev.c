#include <drivers/block/bdev.h>

#include <mm/heap.h>

#include <lib/string.h>

#include <hal/lock.h>

#define BDEV_MAX 16u

typedef struct {
    spinlock_t lock;

    block_device_t* devs[BDEV_MAX];
} bdev_registry_t;

static bdev_registry_t g_bdev;

static block_device_t* g_bdev_root = 0;

void bdev_init(void) {
    memset(&g_bdev, 0, sizeof(g_bdev));
    spinlock_init(&g_bdev.lock);

    g_bdev_root = 0;
}

void bdev_set_root(block_device_t* dev) {
    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock);
    g_bdev_root = dev;
    spinlock_release_safe(&g_bdev.lock, flags);
}

block_device_t* bdev_root(void) {
    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock);
    block_device_t* out = g_bdev_root;
    spinlock_release_safe(&g_bdev.lock, flags);
    return out;
}

static int bdev_is_name_valid(const char* name) {
    if (!name) {
        return 0;
    }

    if (name[0] == '\0') {
        return 0;
    }

    return 1;
}

static int bdev_find_slot_by_name_locked(const char* name) {
    for (uint32_t i = 0; i < BDEV_MAX; i++) {
        block_device_t* d = g_bdev.devs[i];
        if (!d || !d->name) {
            continue;
        }

        if (strcmp(d->name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int bdev_find_free_slot_locked(void) {
    for (uint32_t i = 0; i < BDEV_MAX; i++) {
        if (!g_bdev.devs[i]) {
            return (int)i;
        }
    }

    return -1;
}

static int bdev_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    if (!node || !buffer) {
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
    if (sector_size == 0) {
        return -1;
    }

    if ((offset % sector_size) != 0 || (size % sector_size) != 0) {
        return -1;
    }

    const uint64_t lba = (uint64_t)offset / (uint64_t)sector_size;
    const uint32_t count = size / sector_size;

    if (count == 0) {
        return 0;
    }

    if (!bdev_read_sectors(dev, lba, count, buffer)) {
        return -1;
    }

    return (int)size;
}

static int bdev_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    if (!node || !buffer) {
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
    if (sector_size == 0) {
        return -1;
    }

    if ((offset % sector_size) != 0 || (size % sector_size) != 0) {
        return -1;
    }

    const uint64_t lba = (uint64_t)offset / (uint64_t)sector_size;
    const uint32_t count = size / sector_size;

    if (count == 0) {
        return 0;
    }

    if (!bdev_write_sectors(dev, lba, count, buffer)) {
        return -1;
    }

    return (int)size;
}

static vfs_ops_t g_bdev_ops = {
    .read = bdev_vfs_read,
    .write = bdev_vfs_write,
};

static void bdev_init_node_template(block_device_t* dev) {
    if (!dev) {
        return;
    }

    memset(&dev->node_template, 0, sizeof(dev->node_template));

    if (dev->name) {
        strlcpy(dev->node_template.name, dev->name, sizeof(dev->node_template.name));
    }

    dev->node_template.ops = &g_bdev_ops;
    dev->node_template.private_data = dev;
}

int bdev_register(block_device_t* dev) {
    if (!dev || !dev->ops || !dev->ops->read_sectors) {
        return -1;
    }

    if (!bdev_is_name_valid(dev->name)) {
        return -1;
    }

    if (dev->sector_size == 0 || dev->sector_count == 0) {
        return -1;
    }

    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock);

    if (bdev_find_slot_by_name_locked(dev->name) >= 0) {
        spinlock_release_safe(&g_bdev.lock, flags);
        return -1;
    }

    const int slot = bdev_find_free_slot_locked();
    if (slot < 0) {
        spinlock_release_safe(&g_bdev.lock, flags);
        return -1;
    }

    bdev_init_node_template(dev);

    g_bdev.devs[slot] = dev;

    spinlock_release_safe(&g_bdev.lock, flags);

    devfs_register(&dev->node_template);

    return 0;
}

int bdev_unregister(const char* name) {
    if (!bdev_is_name_valid(name)) {
        return -1;
    }

    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock);

    const int slot = bdev_find_slot_by_name_locked(name);
    if (slot < 0) {
        spinlock_release_safe(&g_bdev.lock, flags);
        return -1;
    }

    g_bdev.devs[slot] = 0;

    spinlock_release_safe(&g_bdev.lock, flags);

    (void)devfs_unregister(name);

    return 0;
}

block_device_t* bdev_find_by_name(const char* name) {
    if (!bdev_is_name_valid(name)) {
        return 0;
    }

    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock);

    const int slot = bdev_find_slot_by_name_locked(name);
    block_device_t* out = (slot >= 0) ? g_bdev.devs[slot] : 0;

    spinlock_release_safe(&g_bdev.lock, flags);

    return out;
}

block_device_t* bdev_first(void) {
    uint32_t flags = spinlock_acquire_safe(&g_bdev.lock);

    block_device_t* out = 0;
    for (uint32_t i = 0; i < BDEV_MAX; i++) {
        if (g_bdev.devs[i]) {
            out = g_bdev.devs[i];
            break;
        }
    }

    spinlock_release_safe(&g_bdev.lock, flags);

    return out;
}

static int bdev_check_bounds(block_device_t* dev, uint64_t lba, uint32_t count) {
    if (!dev) {
        return 0;
    }

    if (count == 0) {
        return 1;
    }

    const uint64_t end = lba + (uint64_t)count;
    if (end < lba) {
        return 0;
    }

    if (end > dev->sector_count) {
        return 0;
    }

    return 1;
}

int bdev_read_sectors(block_device_t* dev, uint64_t lba, uint32_t count, void* buf) {
    if (!dev || !dev->ops || !dev->ops->read_sectors || !buf) {
        return 0;
    }

    if (!bdev_check_bounds(dev, lba, count)) {
        return 0;
    }

    return dev->ops->read_sectors(dev, lba, count, buf) != 0;
}

int bdev_write_sectors(block_device_t* dev, uint64_t lba, uint32_t count, const void* buf) {
    if (!dev || !dev->ops || !dev->ops->write_sectors || !buf) {
        return 0;
    }

    if (!bdev_check_bounds(dev, lba, count)) {
        return 0;
    }

    return dev->ops->write_sectors(dev, lba, count, buf) != 0;
}

int bdev_flush(block_device_t* dev) {
    if (!dev || !dev->ops || !dev->ops->flush) {
        return 1;
    }

    return dev->ops->flush(dev) != 0;
}

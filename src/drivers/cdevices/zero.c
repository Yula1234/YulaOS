#include <drivers/cdev.h>
#include <drivers/driver.h>

#include <lib/string.h>

#include <stdint.h>

static int zero_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;

    if (!buffer || size == 0u) {
        return 0;
    }

    memset(buffer, 0, size);

    return (int)size;
}

static int zero_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;
    (void)buffer;

    return (int)size;
}

static cdevice_t g_zero_cdev = {
    .dev = {
        .name = "zero",
    },
    .ops = {
        .read = zero_read,
        .write = zero_write,
    },
    .node_template = {
        .name = "zero",
    },
};

static int zero_driver_init(void) {
    return cdevice_register(&g_zero_cdev);
}

DRIVER_REGISTER(
    .name = "zero",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = zero_driver_init,
    .shutdown = 0
);

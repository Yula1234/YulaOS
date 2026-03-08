#include <drivers/cdev.h>
#include <drivers/driver.h>

#include <stdint.h>

static int null_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;

    return 0;
}

static int null_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;
    (void)buffer;

    return (int)size;
}

static cdevice_t g_null_cdev = {
    .dev = {
        .name = "null",
    },
    .ops = {
        .read = null_read,
        .write = null_write,
    },
    .node_template = {
        .name = "null",
    },
};

static int null_driver_init(void) {
    return cdevice_register(&g_null_cdev);
}

DRIVER_REGISTER(
    .name = "null",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = null_driver_init,
    .shutdown = 0
);

#include <drivers/cdev.h>
#include <drivers/driver.h>

#include <hal/lock.h>

#include <lib/string.h>

#include <stdint.h>

#define CLIPBOARD_MAX_SIZE 4096u

static uint8_t g_clipboard_data[CLIPBOARD_MAX_SIZE];
static uint32_t g_clipboard_len;

static percpu_rwspinlock_t g_clipboard_lock;

static void clipboard_lock_read(void) {
    percpu_rwspinlock_acquire_read(&g_clipboard_lock);
}

static void clipboard_unlock_read(void) {
    percpu_rwspinlock_release_read(&g_clipboard_lock);
}

static void clipboard_lock_write(void) {
    percpu_rwspinlock_acquire_write(&g_clipboard_lock);
}

static void clipboard_unlock_write(void) {
    percpu_rwspinlock_release_write(&g_clipboard_lock);
}

static uint32_t clipboard_len_locked(void) {
    return g_clipboard_len;
}

static uint32_t clipboard_copy_out_locked(uint32_t offset, void* buffer, uint32_t size) {
    if (!buffer || size == 0u) {
        return 0u;
    }

    const uint32_t len = clipboard_len_locked();
    if (offset >= len) {
        return 0u;
    }

    uint32_t available = len - offset;
    uint32_t to_copy = (size < available) ? size : available;

    memcpy(buffer, &g_clipboard_data[offset], to_copy);
    return to_copy;
}

static uint32_t clipboard_replace_locked(const void* buffer, uint32_t size) {
    if (!buffer || size == 0u) {
        g_clipboard_len = 0u;
        return 0u;
    }

    uint32_t to_copy = size;
    if (to_copy > CLIPBOARD_MAX_SIZE) {
        to_copy = CLIPBOARD_MAX_SIZE;
    }

    memcpy(g_clipboard_data, buffer, to_copy);
    g_clipboard_len = to_copy;

    return to_copy;
}

static int clipboard_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    if (!node) {
        return -1;
    }

    clipboard_lock_read();

    const uint32_t bytes = clipboard_copy_out_locked(offset, buffer, size);
    node->size = clipboard_len_locked();

    clipboard_unlock_read();

    return (int)bytes;
}

static int clipboard_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;

    if (!node) {
        return -1;
    }

    clipboard_lock_write();

    const uint32_t bytes = clipboard_replace_locked(buffer, size);
    node->size = clipboard_len_locked();

    clipboard_unlock_write();

    return (int)bytes;
}

static cdevice_t g_clipboard_cdev = {
    .dev = {
        .name = "clipboard",
    },
    .ops = {
        .read = clipboard_read,
        .write = clipboard_write,
    },
    .node_template = {
        .name = "clipboard",
    },
};

static int clipboard_driver_init(void) {
    percpu_rwspinlock_init(&g_clipboard_lock);

    memset(g_clipboard_data, 0, sizeof(g_clipboard_data));
    g_clipboard_len = 0u;

    g_clipboard_cdev.node_template.size = 0u;

    return cdevice_register(&g_clipboard_cdev);
}

DRIVER_REGISTER(
    .name = "clipboard",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = clipboard_driver_init,
    .shutdown = 0
);

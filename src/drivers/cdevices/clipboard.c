#include <kernel/locking/rwspinlock.h>

#include <drivers/driver.h>
#include <drivers/cdev.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <hal/align.h>

#include <stdint.h>

#define CLIPBOARD_MAX_SIZE 4096u

static __cacheline_aligned uint8_t g_clipboard_data[CLIPBOARD_MAX_SIZE];
static __cacheline_aligned uint32_t g_clipboard_len;

static __cacheline_aligned percpu_rwspinlock_t g_clipboard_lock;

___inline uint32_t clipboard_len_locked(void) {
    return g_clipboard_len;
}

___inline uint32_t clipboard_copy_out_locked(uint32_t offset, void* buffer, uint32_t size) {
    if (unlikely(!buffer
        || size == 0u)) {
        return 0u;
    }

    const uint32_t len = clipboard_len_locked();

    if (unlikely(offset >= len)) {
        return 0u;
    }

    uint32_t available = len - offset;
    uint32_t to_copy = (size < available) ? size : available;

    memcpy(buffer, &g_clipboard_data[offset], to_copy);

    return to_copy;
}

___inline uint32_t clipboard_replace_locked(const void* buffer, uint32_t size) {
    if (unlikely(!buffer
        || size == 0u)) {
        g_clipboard_len = 0u;

        return 0u;
    }

    uint32_t to_copy = size;

    if (unlikely(to_copy > CLIPBOARD_MAX_SIZE)) {
        to_copy = CLIPBOARD_MAX_SIZE;
    }

    memcpy(g_clipboard_data, buffer, to_copy);

    g_clipboard_len = to_copy;

    return to_copy;
}

static int clipboard_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    if (unlikely(!node)) {
        return -1;
    }

    guard(percpu_rwspin_read_safe)(&g_clipboard_lock);

    const uint32_t bytes = clipboard_copy_out_locked(offset, buffer, size);
    node->size = clipboard_len_locked();

    return (int)bytes;
}

static int clipboard_write(vfs_node_t* node, uint32_t ___unused offset, uint32_t size, const void* buffer) {
    if (unlikely(!node)) {
        return -1;
    }

    guard(percpu_rwspin_write_safe)(&g_clipboard_lock);

    const uint32_t bytes = clipboard_replace_locked(buffer, size);
    node->size = clipboard_len_locked();

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

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <drivers/cdev.h>
#include <drivers/driver.h>

#include <hal/pmio.h>

#include <mm/heap.h>

#include <lib/string.h>

#include <stdint.h>


#define IOPORTS_MAX_BUFFER_SIZE 4096u

struct ioports_context {
    char* buffer;
    uint32_t position;
    uint32_t capacity;
};

static void append_hex16(struct ioports_context* context, uint16_t value) {
    if (context->position + 4u >= context->capacity) {
        return;
    }

    const char hex_digits[] = "0123456789ABCDEF";

    context->buffer[context->position++] = hex_digits[(value >> 12) & 0x0Fu];
    context->buffer[context->position++] = hex_digits[(value >> 8) & 0x0Fu];
    context->buffer[context->position++] = hex_digits[(value >> 4) & 0x0Fu];
    context->buffer[context->position++] = hex_digits[value & 0x0Fu];
}

static void append_char(struct ioports_context* context, char c) {
    if (context->position + 1u >= context->capacity) {
        return;
    }

    context->buffer[context->position++] = c;
}

static void append_string(struct ioports_context* context, const char* str) {
    if (!str) {
        return;
    }

    while (*str && context->position + 1u < context->capacity) {
        context->buffer[context->position++] = *str++;
    }
}

static void ioports_iterate_callback(uint16_t start, uint16_t end, const char* name, void* ctx) {
    struct ioports_context* context = (struct ioports_context*)ctx;

    if (!context || !context->buffer) {
        return;
    }

    if (context->position + 64u >= context->capacity) {
        return;
    }

    append_hex16(context, start);
    append_char(context, '-');
    append_hex16(context, end);
    
    append_string(context, " : ");
    append_string(context, name ? name : "unknown");
    append_char(context, '\n');
}

static int ioports_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    if (!node || !buffer) {
        return -1;
    }

    if (size == 0u) {
        return 0;
    }

    char* snapshot_buffer = (char*)kmalloc(IOPORTS_MAX_BUFFER_SIZE);
    if (!snapshot_buffer) {
        return -1;
    }

    struct ioports_context context;
    context.buffer = snapshot_buffer;
    context.position = 0u;
    context.capacity = IOPORTS_MAX_BUFFER_SIZE;

    pmio_iterate(ioports_iterate_callback, &context);

    const uint32_t total_length = context.position;
    node->size = total_length;

    int res = 0;

    if (offset >= total_length) {
        goto out_free;
    }

    const uint32_t available = total_length - offset;
    const uint32_t bytes_to_copy = (size < available) ? size : available;

    memcpy(buffer, snapshot_buffer + offset, bytes_to_copy);
    
    res = (int)bytes_to_copy;

out_free:
    kfree(snapshot_buffer);
    
    return res;
}

static cdevice_t g_ioports_cdev = {
    .dev = {
        .name = "ioports",
    },
    .ops = {
        .read = ioports_read,
    },
    .node_template = {
        .name = "ioports",
    },
};

static int ioports_driver_init(void) {
    g_ioports_cdev.node_template.size = 0u;

    return cdevice_register(&g_ioports_cdev);
}

DRIVER_REGISTER(
    .name = "ioports",
    .klass = DRIVER_CLASS_CHAR,
    .stage = DRIVER_STAGE_VFS,
    .init = ioports_driver_init,
    .shutdown = 0
);
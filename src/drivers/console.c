// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>

#include <hal/irq.h>
#include <fs/vfs.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/window.h>
#include <hal/lock.h>

#include "console.h"
#include "vga.h"


static int console_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;  
    (void)offset;
    
    task_t* curr = proc_current();
    if (!curr->terminal) return -1;

    term_instance_t* term = (term_instance_t*)curr->terminal;

    const char* char_buf = (const char*)buffer;

    const uint32_t BATCH = 256;
    uint32_t pos = 0;

    while (pos < size) {
        uint32_t end = pos + BATCH;
        if (end > size) end = size;

        spinlock_acquire(&term->lock);

        term_write(term, &char_buf[pos], end - pos);

        spinlock_release(&term->lock);
        pos = end;
    }

    window_mark_dirty_by_pid_pair((int)curr->pid, (int)curr->parent_pid);
    
    return size;
}

static vfs_ops_t console_ops = { .write = console_vfs_write };
static vfs_node_t console_node = { .name = "console", .ops = &console_ops };

void console_init() {
    devfs_register(&console_node);
}
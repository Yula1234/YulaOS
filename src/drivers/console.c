// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>

#include <hal/irq.h>
#include <fs/vfs.h>
#include <kernel/proc.h>
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
    
    uint32_t flags = spinlock_acquire_safe(&term->lock);
    
    for (uint32_t i = 0; i < size; i++) {
        term_putc(term, char_buf[i]);
    }
    
    spinlock_release_safe(&term->lock, flags);

    extern void window_mark_dirty_by_pid(int pid);
    
    window_mark_dirty_by_pid(curr->pid);
    
    if (curr->parent_pid > 0) {
        window_mark_dirty_by_pid(curr->parent_pid);
    }
    
    return size;
}

static vfs_ops_t console_ops = { .write = console_vfs_write };
static vfs_node_t console_node = { .name = "console", .ops = &console_ops };

void console_init() {
    devfs_register(&console_node);
}
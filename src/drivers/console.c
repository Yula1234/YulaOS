#include <stdint.h>

#include <hal/irq.h>
#include <fs/vfs.h>
#include <kernel/proc.h>

#include "console.h"
#include "vga.h"


static int console_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;  
    (void)offset;
    
    task_t* curr = proc_current();
    if (!curr->terminal) return -1;

    const char* char_buf = (const char*)buffer;
    for (uint32_t i = 0; i < size; i++) {
        term_putc((term_instance_t*)curr->terminal, char_buf[i]);
    }

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
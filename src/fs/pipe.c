#include <kernel/sched.h>
#include <lib/string.h>
#include <hal/lock.h>
#include <mm/heap.h>

#include "vfs.h"

#define PIPE_SIZE 4096

typedef struct {
    char buffer[PIPE_SIZE];
    uint32_t read_ptr;
    uint32_t write_ptr;
    uint32_t available;
    
    int readers;
    int writers;
    
    spinlock_t lock;
} pipe_t;

static int pipe_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    char* buf = (char*)buffer;
    uint32_t read_count = 0;
    task_t* curr = proc_current();

    while (read_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);
        
        if (p->available == 0) {
            if (p->writers == 0) {
                spinlock_release_safe(&p->lock, flags);
                return read_count;
            }

            if (read_count > 0) {
                spinlock_release_safe(&p->lock, flags);
                return read_count;
            }

            spinlock_release_safe(&p->lock, flags);
            
            if (curr->pending_signals != 0) {
                return -2;
            }

            sched_yield(); 
            continue;
        }

        buf[read_count] = p->buffer[p->read_ptr % PIPE_SIZE];
        p->read_ptr++;
        p->available--;
        read_count++;
        
        spinlock_release_safe(&p->lock, flags);
    }
    return read_count;
}

static int pipe_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    const char* buf = (const char*)buffer;
    uint32_t written_count = 0;

    while (written_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);

        if (p->readers == 0) {
            spinlock_release_safe(&p->lock, flags);
            return -1;
        }

        if (p->available == PIPE_SIZE) {
            spinlock_release_safe(&p->lock, flags);
            sched_yield();
            continue;
        }

        p->buffer[p->write_ptr % PIPE_SIZE] = buf[written_count];
        p->write_ptr++;
        p->available++;
        written_count++;

        spinlock_release_safe(&p->lock, flags);
    }
    return written_count;
}

static int pipe_close(vfs_node_t* node) {
    pipe_t* p = (pipe_t*)node->private_data;
    
    uint32_t flags = spinlock_acquire_safe(&p->lock);
    if (node->flags == 1) p->readers--;
    else if (node->flags == 2) p->writers--;
    
    int total = p->readers + p->writers;
    spinlock_release_safe(&p->lock, flags);

    if (total == 0) {
        kfree(p);
    }
    kfree(node);
    return 0;
}

static vfs_ops_t pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .open = 0
};

int vfs_create_pipe(vfs_node_t** read_node, vfs_node_t** write_node) {
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return -1;
    
    memset(p, 0, sizeof(pipe_t));
    spinlock_init(&p->lock);
    p->readers = 1;
    p->writers = 1;

    *read_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    *write_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));

    strlcpy((*read_node)->name, "pipe_r", 32);
    (*read_node)->ops = &pipe_ops;
    (*read_node)->private_data = p;
    (*read_node)->inode_idx = 0;
    (*read_node)->flags = 1;

    strlcpy((*write_node)->name, "pipe_w", 32);
    (*write_node)->ops = &pipe_ops;
    (*write_node)->private_data = p;
    (*write_node)->inode_idx = 0;
    (*write_node)->flags = 2;

    (*read_node)->refs = 1; 
    (*write_node)->refs = 1;

    return 0;
}
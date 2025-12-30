// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

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
    
    semaphore_t sem_read; 
    semaphore_t sem_write;
    
    int readers;
    int writers;
    
    spinlock_t lock;
} pipe_t;

static int pipe_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    char* buf = (char*)buffer;
    uint32_t read_count = 0;

    while (read_count < size) {
        sem_wait(&p->sem_read);

        uint32_t flags = spinlock_acquire_safe(&p->lock);
        
        uint32_t available = p->write_ptr - p->read_ptr;
        
        if (available == 0) {
            if (p->writers == 0) {
                sem_signal(&p->sem_read); 
                spinlock_release_safe(&p->lock, flags);
                return (int)read_count;
            }
             spinlock_release_safe(&p->lock, flags);
             continue;
        }

        buf[read_count] = p->buffer[p->read_ptr % PIPE_SIZE];
        p->read_ptr++;
        read_count++;
        
        spinlock_release_safe(&p->lock, flags);
        
        sem_signal(&p->sem_write);
    }
    return (int)read_count;
}

static int pipe_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    const char* buf = (const char*)buffer;
    uint32_t written_count = 0;

    while (written_count < size) {
        sem_wait(&p->sem_write);
        
        uint32_t flags = spinlock_acquire_safe(&p->lock);
        
        if (p->readers == 0) {
            spinlock_release_safe(&p->lock, flags);
            return (written_count > 0) ? (int)written_count : -1;
        }

        p->buffer[p->write_ptr % PIPE_SIZE] = buf[written_count];
        p->write_ptr++;
        written_count++;

        spinlock_release_safe(&p->lock, flags);
        
        sem_signal(&p->sem_read);
    }
    return (int)written_count;
}

static int pipe_close(vfs_node_t* node) {
    pipe_t* p = (pipe_t*)node->private_data;
    
    uint32_t flags = spinlock_acquire_safe(&p->lock);
    if (node->flags == 1) p->readers--;
    else if (node->flags == 2) p->writers--;
    
    int readers = p->readers;
    int writers = p->writers;
    spinlock_release_safe(&p->lock, flags);

    if (node->flags == 1) { 
        sem_signal(&p->sem_write); // Wake up writers so they see Broken Pipe
    } else {
        sem_signal(&p->sem_read);  // Wake up readers so they see EOF
    }

    if (readers == 0 && writers == 0) {
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
    
    sem_init(&p->sem_read, 0);          
    sem_init(&p->sem_write, PIPE_SIZE); 
    
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
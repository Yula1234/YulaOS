// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <kernel/sched.h>
#include <lib/string.h>
#include <hal/lock.h>
#include <mm/heap.h>

#include "vfs.h"

#define PIPE_SIZE 4096

#define PIPE_BATCH 128

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

static uint32_t sem_take_up_to(semaphore_t* sem, uint32_t max) {
    if (max == 0) return 0;

    sem_wait(sem);

    uint32_t taken = 1;
    while (taken < max && sem_try_acquire(sem)) {
        taken++;
    }

    return taken;
}

static void sem_give_n(semaphore_t* sem, uint32_t n) {
    while (n--) {
        sem_signal(sem);
    }
}

static int pipe_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    char* buf = (char*)buffer;
    uint32_t read_count = 0;

    while (read_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);
        uint32_t available = p->write_ptr - p->read_ptr;
        int writers = p->writers;
        spinlock_release_safe(&p->lock, flags);

        if (available == 0 && writers == 0) {
            return (int)read_count;
        }

        uint32_t want = size - read_count;
        if (want > PIPE_BATCH) want = PIPE_BATCH;

        uint32_t take = sem_take_up_to(&p->sem_read, want);

        flags = spinlock_acquire_safe(&p->lock);
        uint32_t now_avail = p->write_ptr - p->read_ptr;

        if (now_avail == 0 && p->writers == 0) {
            spinlock_release_safe(&p->lock, flags);
            sem_give_n(&p->sem_read, take);
            return (int)read_count;
        }

        uint32_t n = take;
        if (n > now_avail) n = now_avail;

        uint32_t rp = p->read_ptr % PIPE_SIZE;
        uint32_t contig = PIPE_SIZE - rp;

        uint32_t n1 = n;
        if (n1 > contig) n1 = contig;
        memcpy(&buf[read_count], &p->buffer[rp], n1);
        p->read_ptr += n1;
        read_count += n1;

        uint32_t n2 = n - n1;
        if (n2 > 0) {
            memcpy(&buf[read_count], &p->buffer[0], n2);
            p->read_ptr += n2;
            read_count += n2;
        }

        spinlock_release_safe(&p->lock, flags);

        if (n < take) {
            sem_give_n(&p->sem_read, take - n);
        }
        sem_give_n(&p->sem_write, n);
    }
    return (int)read_count;
}

static int pipe_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    const char* buf = (const char*)buffer;
    uint32_t written_count = 0;

    while (written_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);
        int readers = p->readers;
        spinlock_release_safe(&p->lock, flags);

        if (readers == 0) {
            return (written_count > 0) ? (int)written_count : -1;
        }

        uint32_t want = size - written_count;
        if (want > PIPE_BATCH) want = PIPE_BATCH;

        uint32_t take = sem_take_up_to(&p->sem_write, want);

        flags = spinlock_acquire_safe(&p->lock);
        if (p->readers == 0) {
            spinlock_release_safe(&p->lock, flags);
            sem_give_n(&p->sem_write, take);
            return (written_count > 0) ? (int)written_count : -1;
        }

        uint32_t n = take;
        uint32_t wp = p->write_ptr % PIPE_SIZE;
        uint32_t contig = PIPE_SIZE - wp;

        uint32_t n1 = n;
        if (n1 > contig) n1 = contig;
        memcpy(&p->buffer[wp], &buf[written_count], n1);
        p->write_ptr += n1;
        written_count += n1;

        uint32_t n2 = n - n1;
        if (n2 > 0) {
            memcpy(&p->buffer[0], &buf[written_count], n2);
            p->write_ptr += n2;
            written_count += n2;
        }

        spinlock_release_safe(&p->lock, flags);

        sem_give_n(&p->sem_read, n);
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
        sem_signal(&p->sem_write);
    } else {
        sem_signal(&p->sem_read);
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
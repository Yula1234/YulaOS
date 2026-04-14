/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/waitq/poll_waitq.h>
#include <kernel/uaccess/uaccess.h>

#include <kernel/locking/mutex.h>
#include <kernel/locking/sem.h>

#include <kernel/sched.h>
#include <kernel/proc.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <mm/heap.h>

#include "pipe.h"

#define PIPE_SIZE 32768u

/*
 * Pipe core.
 *
 * This is a byte-stream pipe implemented as a fixed-size
 * ring buffer.
 *
 * Concurrency model:
 *  - `lock` is a sleeping mutex that protects the ring indices, reader/writer 
 *    counters, and allows safe `uaccess` copy operations while held.
 *  - I/O coordination acts like a Condition Variable pattern using semaphores:
 *      sem_read  == Signaled when new data becomes available.
 *      sem_write == Signaled when new free space becomes available.
 *
 * Invariants:
 *  - write_ptr and read_ptr are monotonic counters; the ring index is derived
 *    via modulo.
 *  - write_ptr - read_ptr is the current occupancy and is bounded by `size`.
 *  - Operations re-check conditions after blocking because state can change
 *    (e.g., endpoints closed) during the sleep phase.
 */

typedef struct {
    /* cacheline 1 core hot data protected by `lock`*/
    mutex_t lock __cacheline_aligned;

    char* buffer;
    uint32_t size;
    
    uint32_t read_ptr;
    uint32_t write_ptr;

    /* cacheline 2 poll subsystem */
    poll_waitq_t poll_waitq __cacheline_aligned;

    /* cacheline 3-4 reader synchronization */
    semaphore_t sem_read __cacheline_aligned;

    /* cacheline 5-6 writer synchronization */
    semaphore_t sem_write __cacheline_aligned;

    /* cacheline 7 cold data & lifecycle */
    volatile uint32_t refs __cacheline_aligned;
    
    int readers;
    int writers;

} pipe_t;

static void pipe_poll_waitq_finalize(void* ctx) {
    pipe_t* p = (pipe_t*)ctx;

    if (!p) {
        return;
    }

    if (p->buffer) {
        kfree(p->buffer);
        p->buffer = 0;
    }

    kfree(p);
}

static void pipe_private_retain(void* private_data) {
    pipe_t* p = (pipe_t*)private_data;

    if (!p) {
        return;
    }

    __atomic_fetch_add(&p->refs, 1u, __ATOMIC_RELAXED);
}

static void pipe_private_release(void* private_data) {
    pipe_t* p = (pipe_t*)private_data;

    if (!p) {
        return;
    }

    if (__atomic_sub_fetch(&p->refs, 1u, __ATOMIC_ACQ_REL) != 0u) {
        return;
    }

    poll_waitq_detach_all(&p->poll_waitq);
    poll_waitq_put(&p->poll_waitq);
}

static int pipe_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;

    if (!node || !buffer || size == 0u) {
        return 0;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    uint32_t read_count = 0u;
    char* buf = (char*)buffer;

    mutex_lock(&p->lock);

    while (read_count < size) {
        /*
         * Wait for data to become available.
         * Drop the lock, sleep on the condition, and drain spurious wakeups.
         */
        while (p->write_ptr == p->read_ptr 
               && p->writers > 0) {
               
            if (read_count > 0u) {
                goto out;
            }

            mutex_unlock(&p->lock);

            sem_wait(&p->sem_read);

            mutex_lock(&p->lock);

            while (sem_try_acquire(&p->sem_read)) {}
        }

        if (p->write_ptr == p->read_ptr 
            && p->writers == 0) {
            break;
        }

        const uint32_t available = p->write_ptr - p->read_ptr;
        const uint32_t want = size - read_count;
        const uint32_t take = (want < available) ? want : available;

        const uint32_t rp = p->read_ptr % p->size;
        const uint32_t contig = p->size - rp;

        const uint32_t n1 = (take < contig) ? take : contig;

        memcpy(&buf[read_count], &p->buffer[rp], n1);

        const uint32_t n2 = take - n1;

        if (n2 > 0u) {
            memcpy(&buf[read_count + n1], &p->buffer[0], n2);
        }

        p->read_ptr += take;
        read_count += take;

        sem_signal_all(&p->sem_write);
        poll_waitq_wake_all(&p->poll_waitq);

        if (read_count > 0u) {
            break;
        }
    }

out:
    /* Leave the door open for other readers if there is still data. */
    if (p->write_ptr > p->read_ptr) {
        sem_signal_all(&p->sem_read);
    }

    mutex_unlock(&p->lock);

    return (int)read_count;
}

int pipe_read_nonblock(vfs_node_t* node, uint32_t size, void* buffer) {
    if (!node || !buffer || size == 0u) {
        return 0;
    }

    if ((node->flags & VFS_FLAG_PIPE_READ) == 0u) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    char* buf = (char*)buffer;

    mutex_lock(&p->lock);

    if (p->write_ptr == p->read_ptr) {
        int res = (p->writers > 0) ? 0 : -1;
        
        mutex_unlock(&p->lock);
        
        return res;
    }

    const uint32_t available = p->write_ptr - p->read_ptr;
    const uint32_t take = (size < available) ? size : available;

    const uint32_t rp = p->read_ptr % p->size;
    const uint32_t contig = p->size - rp;
    
    const uint32_t n1 = (take < contig) ? take : contig;

    if (uaccess_copy_to_user(&buf[0], &p->buffer[rp], n1) != 0) {
        mutex_unlock(&p->lock);
        
        return -1;
    }

    const uint32_t n2 = take - n1;

    if (n2 > 0u) {
        if (uaccess_copy_to_user(&buf[n1], &p->buffer[0], n2) != 0) {
            mutex_unlock(&p->lock);
            
            return -1;
        }
    }

    p->read_ptr += take;

    sem_signal_all(&p->sem_write);
    poll_waitq_wake_all(&p->poll_waitq);

    if (p->write_ptr > p->read_ptr) {
        sem_signal_all(&p->sem_read);
    }

    mutex_unlock(&p->lock);

    return (int)take;
}

static int pipe_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;

    if (!node || !buffer || size == 0u) {
        return 0;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    uint32_t written_count = 0u;
    const char* buf = (const char*)buffer;

    /*
     * Guarantee atomic writes if the requested size fits within the total
     * pipe capacity, preventing interleaving.
     */
    const uint32_t require_space = (size <= p->size) ? size : 1u;

    mutex_lock(&p->lock);

    while (written_count < size) {
        while (p->size - (p->write_ptr - p->read_ptr) < require_space 
               && p->readers > 0) {
               
            mutex_unlock(&p->lock);

            sem_wait(&p->sem_write);

            mutex_lock(&p->lock);

            while (sem_try_acquire(&p->sem_write)) {}
        }

        if (p->readers == 0) {
            if (written_count == 0u) {
                written_count = (uint32_t)-1;
            }

            break;
        }

        const uint32_t space = p->size - (p->write_ptr - p->read_ptr);
        const uint32_t want = size - written_count;
        const uint32_t take = (want < space) ? want : space;

        const uint32_t wp = p->write_ptr % p->size;
        const uint32_t contig = p->size - wp;

        const uint32_t n1 = (take < contig) ? take : contig;

        memcpy(&p->buffer[wp], &buf[written_count], n1);

        const uint32_t n2 = take - n1;

        if (n2 > 0u) {
            memcpy(&p->buffer[0], &buf[written_count + n1], n2);
        }

        p->write_ptr += take;
        written_count += take;

        sem_signal_all(&p->sem_read);
        poll_waitq_wake_all(&p->poll_waitq);
    }

    /* Chain-wake other writers if space remains. */
    if (p->size - (p->write_ptr - p->read_ptr) > 0u) {
        sem_signal_all(&p->sem_write);
    }

    mutex_unlock(&p->lock);

    return (int)written_count;
}

int pipe_write_nonblock(vfs_node_t* node, uint32_t size, const void* buffer) {
    if (!node || !buffer || size == 0u) {
        return 0;
    }

    if ((node->flags & VFS_FLAG_PIPE_WRITE) == 0u) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    if (size > p->size) {
        return 0;
    }

    const char* buf = (const char*)buffer;

    mutex_lock(&p->lock);

    if (p->readers == 0) {
        mutex_unlock(&p->lock);
        
        return -1;
    }

    const uint32_t space = p->size - (p->write_ptr - p->read_ptr);
    
    if (space < size) {
        mutex_unlock(&p->lock);
        
        return 0;
    }

    const uint32_t take = size;
    const uint32_t wp = p->write_ptr % p->size;
    const uint32_t contig = p->size - wp;
    
    const uint32_t n1 = (take < contig) ? take : contig;

    if (uaccess_copy_from_user(&p->buffer[wp], &buf[0], n1) != 0) {
        mutex_unlock(&p->lock);
        
        return -1;
    }

    const uint32_t n2 = take - n1;

    if (n2 > 0u) {
        if (uaccess_copy_from_user(&p->buffer[0], &buf[n1], n2) != 0) {
            mutex_unlock(&p->lock);
            
            return -1;
        }
    }

    p->write_ptr += take;

    sem_signal_all(&p->sem_read);
    poll_waitq_wake_all(&p->poll_waitq);

    if (p->size - (p->write_ptr - p->read_ptr) > 0u) {
        sem_signal_all(&p->sem_write);
    }

    mutex_unlock(&p->lock);

    return (int)take;
}

int pipe_poll_info(
    vfs_node_t* node, uint32_t* out_available, uint32_t* out_space,
    int* out_readers, int* out_writers
) {
    if (out_available)
        *out_available = 0;
    
    if (out_space)
        *out_space = 0;
    
    if (out_readers)
        *out_readers = 0;
    
    if (out_writers)
        *out_writers = 0;

    if (!node) {
        return -1;
    }

    if ((node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) == 0u) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    mutex_lock(&p->lock);

    const uint32_t available = p->write_ptr - p->read_ptr;

    const uint32_t space = p->size - available;

    const int readers = p->readers;
    const int writers = p->writers;

    mutex_unlock(&p->lock);

    if (out_available)
        *out_available = available;
    
    if (out_space)
        *out_space = space;
    
    if (out_readers)
        *out_readers = readers;
    
    if (out_writers)
        *out_writers = writers;

    return 0;
}

int pipe_poll_waitq_register(vfs_node_t* node, poll_waiter_t* w, task_t* task) {
    if (!node || !w || !task) {
        return -1;
    }

    if ((node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) == 0u) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    return poll_waitq_register(&p->poll_waitq, w, task);
}

static int pipe_vfs_poll_status(vfs_node_t* node, int events) {
    if (!node) {
        return 0;
    }

    if ((node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) == 0u) {
        return 0;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return VFS_POLLHUP;
    }

    uint32_t avail = 0;
    uint32_t space = 0;
    int readers = 0;
    int writers = 0;

    if (pipe_poll_info(node, &avail, &space, &readers, &writers) != 0) {
        return VFS_POLLERR;
    }

    int rev = 0;

    if (node->flags & VFS_FLAG_PIPE_READ) {
        if ((events & VFS_POLLIN) && avail > 0u) {
            rev |= VFS_POLLIN;
        }

        if (writers == 0) {
            rev |= VFS_POLLHUP;
        }
    }

    if (node->flags & VFS_FLAG_PIPE_WRITE) {
        if ((events & VFS_POLLOUT) && readers > 0 && space > 0u) {
            rev |= VFS_POLLOUT;
        }

        if (readers == 0) {
            rev |= VFS_POLLHUP;
        }
    }

    return rev;
}

static int pipe_vfs_poll_register(vfs_node_t* node, poll_waiter_t* w, task_t* task) {
    return pipe_poll_waitq_register(node, w, task);
}

static int pipe_close(vfs_node_t* node) {
    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return 0;
    }

    mutex_lock(&p->lock);

    if (node->flags & VFS_FLAG_PIPE_READ) {
        p->readers--;
    } else if (node->flags & VFS_FLAG_PIPE_WRITE) {
        p->writers--;
    }

    const int readers = p->readers;
    const int writers = p->writers;

    /* Wake all blocked users so they can observe the state change. */
    sem_signal_all(&p->sem_read);
    sem_signal_all(&p->sem_write);

    poll_waitq_wake_all(&p->poll_waitq);

    mutex_unlock(&p->lock);

    if (readers == 0 && writers == 0) {
        poll_waitq_detach_all(&p->poll_waitq);
    }

    return 0;
}

static vfs_ops_t pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .open = 0,
    .close = pipe_close,
    .ioctl = 0,
    .get_phys_page = 0,
    .poll_status = pipe_vfs_poll_status,
    .poll_register = pipe_vfs_poll_register,
};

int vfs_create_pipe(vfs_node_t** read_node, vfs_node_t** write_node) {
    if (read_node) {
        *read_node = 0;
    }

    if (write_node) {
        *write_node = 0;
    }

    if (!read_node || !write_node) {
        return -1;
    }

    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));

    if (!p) {
        return -1;
    }

    memset(p, 0, sizeof(pipe_t));

    p->refs = 2u;
    p->size = PIPE_SIZE;

    p->buffer = (char*)kmalloc(p->size);

    if (!p->buffer) {
        kfree(p);
        
        return -1;
    }

    mutex_init(&p->lock);

    sem_init(&p->sem_read, 0);
    sem_init(&p->sem_write, 0);

    p->readers = 1;
    p->writers = 1;

    *read_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    *write_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));

    if (!*read_node || !*write_node) {
        if (*read_node) {
            kfree(*read_node);
        }

        if (*write_node) {
            kfree(*write_node);
        }

        *read_node = 0;
        *write_node = 0;

        kfree(p->buffer);
        kfree(p);

        return -1;
    }

    memset(*read_node, 0, sizeof(**read_node));
    memset(*write_node, 0, sizeof(**write_node));

    strlcpy((*read_node)->name, "pipe_r", 32);
    (*read_node)->ops = &pipe_ops;
    (*read_node)->private_data = p;
    (*read_node)->private_retain = pipe_private_retain;
    (*read_node)->private_release = pipe_private_release;
    (*read_node)->inode_idx = 0;
    (*read_node)->size = 0;
    (*read_node)->flags = VFS_FLAG_PIPE_READ;
    (*read_node)->refs = 1u;

    strlcpy((*write_node)->name, "pipe_w", 32);
    (*write_node)->ops = &pipe_ops;
    (*write_node)->private_data = p;
    (*write_node)->private_retain = pipe_private_retain;
    (*write_node)->private_release = pipe_private_release;
    (*write_node)->inode_idx = 0;
    (*write_node)->size = 0;
    (*write_node)->flags = VFS_FLAG_PIPE_WRITE;
    (*write_node)->refs = 1u;

    poll_waitq_init_finalizable(&p->poll_waitq, pipe_poll_waitq_finalize, p);

    return 0;
}
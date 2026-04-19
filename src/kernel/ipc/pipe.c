/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/waitq/poll_waitq.h>
#include <kernel/uaccess/uaccess.h>

#include <kernel/locking/guards.h>
#include <kernel/locking/mutex.h>
#include <kernel/locking/sem.h>

#include <kernel/sched.h>
#include <kernel/proc.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <hal/align.h>

#include <mm/heap.h>

#include "pipe.h"

#define PIPE_SIZE 32768u

/*
 * Pipe Core.
 *
 * This implementation uses a Single-Producer / Single-Consumer (SPSC) 
 * lock-free ring buffer under the hood, protected by independent mutexes 
 * to serialize multiple readers and multiple writers.
 *
 * Concurrency & Cache Model:
 *  - Readers and writers operate on separate cache lines.
 *  - `read_lock` serializes readers. `write_lock` serializes writers.
 *  - Because only 1 reader and 1 writer can actively touch the ring at a time,
 *    they synchronize purely via atomic loads/stores of `read_ptr` and `write_ptr`,
 *    allowing completely lock-free concurrent data copying.
 *  - Standard VFS paths (`pipe_read`/`pipe_write`) use fast `memcpy` because 
 *    the VFS layer already safely buffered the data into kernel space.
 *  - Non-blocking syscall paths use `uaccess` directly.
 */

typedef struct {
    mutex_t read_lock __cacheline_aligned;

    volatile uint32_t read_ptr;
    volatile int readers;

    mutex_t write_lock __cacheline_aligned;
    
    volatile uint32_t write_ptr;
    volatile int writers;

    semaphore_t sem_read __cacheline_aligned;

    semaphore_t sem_write __cacheline_aligned;

    poll_waitq_t poll_waitq __cacheline_aligned;

    char* buffer __cacheline_aligned;

    uint32_t size;
    
    volatile uint32_t refs;
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

___inline int pipe_read_impl(vfs_node_t* node, uint32_t size, void* buffer, int block, int is_user) {
    if (!node || !buffer || size == 0u) {
        return 0;
    }

    if (!block && (node->flags & VFS_FLAG_PIPE_READ) == 0u) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;
    
    if (!p) {
        return -1;
    }

    char* buf = (char*)buffer;
    uint32_t read_count = 0u;

    while (read_count < size) {
        bool waited = false;

        for (;;) {
            {
                guard_mutex(&p->read_lock);

                if (waited) {
                    while (sem_try_acquire(&p->sem_read)) {}
                }

                const uint32_t wp = __atomic_load_n(&p->write_ptr, __ATOMIC_ACQUIRE);

                const uint32_t rp = p->read_ptr;
                const uint32_t available = wp - rp;

                if (available > 0) {
                    const uint32_t want = size - read_count;
                    const uint32_t take = (want < available) ? want : available;

                    const uint32_t rp_mod = rp % p->size;
                    const uint32_t contig = p->size - rp_mod;

                    const uint32_t n1 = (take < contig) ? take : contig;

                    if (is_user) {
                        if (uaccess_copy_to_user(&buf[read_count], &p->buffer[rp_mod], n1) != 0) {
                            if (read_count == 0) {
                                read_count = (uint32_t)-1;
                            }
                            
                            if (__atomic_load_n(&p->write_ptr, __ATOMIC_ACQUIRE) > rp) {
                                sem_signal_all(&p->sem_read);
                            }

                            return (int)read_count;
                        }
                    } else {
                        memcpy(&buf[read_count], &p->buffer[rp_mod], n1);
                    }

                    const uint32_t n2 = take - n1;
                    if (n2 > 0u) {
                        if (is_user) {
                            if (uaccess_copy_to_user(&buf[read_count + n1], &p->buffer[0], n2) != 0) {
                                __atomic_store_n(&p->read_ptr, rp + n1, __ATOMIC_RELEASE);
                                
                                sem_signal_all(&p->sem_write);

                                poll_waitq_wake_all(&p->poll_waitq);
                                
                                if (__atomic_load_n(&p->write_ptr, __ATOMIC_ACQUIRE) > (rp + n1)) {
                                    sem_signal_all(&p->sem_read);
                                }

                                return -1;
                            }
                        } else {
                            memcpy(&buf[read_count + n1], &p->buffer[0], n2);
                        }
                    }

                    __atomic_store_n(&p->read_ptr, rp + take, __ATOMIC_RELEASE);
                    read_count += take;

                    sem_signal_all(&p->sem_write);

                    poll_waitq_wake_all(&p->poll_waitq);

                    if (__atomic_load_n(&p->write_ptr, __ATOMIC_ACQUIRE) > (rp + take)) {
                        sem_signal_all(&p->sem_read);
                    }

                    break;
                }

                if (__atomic_load_n(&p->writers, __ATOMIC_ACQUIRE) == 0) {
                    return (!block && read_count == 0) ? -1 : (int)read_count;
                }

                if (!block) {
                    return 0;
                }

            }

            sem_wait(&p->sem_read);

            waited = true;
        }

        if (read_count > 0u) {
            break;
        }
    }

    return (int)read_count;
}

___inline int pipe_write_impl(vfs_node_t* node, uint32_t size, const void* buffer, int block, int is_user) {
    if (!node || !buffer || size == 0u) {
        return 0;
    }

    pipe_t* p = (pipe_t*)node->private_data;

    if (!p) {
        return -1;
    }

    if (!block) {
        if ((node->flags & VFS_FLAG_PIPE_WRITE) == 0u) {
            return -1;
        }

        if (size > p->size) {
            return 0;
        }
    }

    const char* buf = (const char*)buffer;
    uint32_t written_count = 0u;

    const uint32_t require_space = (!block || size <= p->size) ? size : 1u;

    while (written_count < size) {
        bool waited = false;

        for (;;) {
            {
                guard_mutex(&p->write_lock);

                if (waited) {
                    while (sem_try_acquire(&p->sem_write)) {}
                }

                const uint32_t rp = __atomic_load_n(&p->read_ptr, __ATOMIC_ACQUIRE);
                
                const uint32_t wp = p->write_ptr;
                const uint32_t space = p->size - (wp - rp);

                if (__atomic_load_n(&p->readers, __ATOMIC_ACQUIRE) == 0) {
                    return (written_count == 0u) ? -1 : (int)written_count;
                }

                if (space >= require_space) {
                    const uint32_t want = size - written_count;
                    const uint32_t take = (!block) ? want : ((want < space) ? want : space);

                    const uint32_t wp_mod = wp % p->size;
                    const uint32_t contig = p->size - wp_mod;

                    const uint32_t n1 = (take < contig) ? take : contig;

                    if (is_user) {
                        if (uaccess_copy_from_user(&p->buffer[wp_mod], &buf[written_count], n1) != 0) {
                            if (p->size - (wp - __atomic_load_n(&p->read_ptr, __ATOMIC_ACQUIRE)) > 0u) {
                                sem_signal_all(&p->sem_write);
                            }

                            return (written_count == 0) ? -1 : (int)written_count;
                        }
                    } else {
                        memcpy(&p->buffer[wp_mod], &buf[written_count], n1);
                    }

                    const uint32_t n2 = take - n1;
                    if (n2 > 0u) {
                        if (is_user) {
                            if (uaccess_copy_from_user(&p->buffer[0], &buf[written_count + n1], n2) != 0) {
                                __atomic_store_n(&p->write_ptr, wp + n1, __ATOMIC_RELEASE);
                                
                                sem_signal_all(&p->sem_read);

                                poll_waitq_wake_all(&p->poll_waitq);

                                if (p->size - ((wp + n1) - __atomic_load_n(&p->read_ptr, __ATOMIC_ACQUIRE)) > 0u) {
                                    sem_signal_all(&p->sem_write);
                                }
                                return -1;
                            }
                        } else {
                            memcpy(&p->buffer[0], &buf[written_count + n1], n2);
                        }
                    }

                    __atomic_store_n(&p->write_ptr, wp + take, __ATOMIC_RELEASE);

                    written_count += take;

                    sem_signal_all(&p->sem_read);

                    poll_waitq_wake_all(&p->poll_waitq);

                    if (p->size - ((wp + take) - __atomic_load_n(&p->read_ptr, __ATOMIC_ACQUIRE)) > 0u) {
                        sem_signal_all(&p->sem_write);
                    }

                    break;
                }

                if (!block) {
                    return 0;
                }

            }

            sem_wait(&p->sem_write);

            waited = true;
        }

        if (!block) {
            break;
        }
    }

    return (int)written_count;
}

static int pipe_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;
    return pipe_read_impl(node, size, buffer, 1, 0);
}

int pipe_read_nonblock(vfs_node_t* node, uint32_t size, void* buffer) {
    return pipe_read_impl(node, size, buffer, 0, 1);
}

static int pipe_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;
    return pipe_write_impl(node, size, buffer, 1, 0);
}

int pipe_write_nonblock(vfs_node_t* node, uint32_t size, const void* buffer) {
    return pipe_write_impl(node, size, buffer, 0, 1);
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

    /* 
     * The atomic guarantees ensure we get a consistent enough snapshot 
     * for event loop triggers.
     */
    const uint32_t wp = __atomic_load_n(&p->write_ptr, __ATOMIC_ACQUIRE);
    const uint32_t rp = __atomic_load_n(&p->read_ptr, __ATOMIC_ACQUIRE);

    const uint32_t available = wp - rp;
    const uint32_t space = p->size - available;

    if (out_available)
        *out_available = available;
    
    if (out_space)
        *out_space = space;
    
    if (out_readers)
        *out_readers = __atomic_load_n(&p->readers, __ATOMIC_ACQUIRE);
    
    if (out_writers)
        *out_writers = __atomic_load_n(&p->writers, __ATOMIC_ACQUIRE);

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

    int readers_left = 1;
    int writers_left = 1;

    if (node->flags & VFS_FLAG_PIPE_READ) {
        readers_left = __atomic_sub_fetch(&p->readers, 1, __ATOMIC_ACQ_REL);
        
        /* Wake writers so they hit EPIPE immediately */
        sem_signal_all(&p->sem_write);
    } else if (node->flags & VFS_FLAG_PIPE_WRITE) {
        writers_left = __atomic_sub_fetch(&p->writers, 1, __ATOMIC_ACQ_REL);
        
        /* Wake readers so they hit EOF */
        sem_signal_all(&p->sem_read);
    }

    poll_waitq_wake_all(&p->poll_waitq);

    if (readers_left == 0 && writers_left == 0) {
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

    mutex_init(&p->read_lock);
    mutex_init(&p->write_lock);

    sem_init(&p->sem_read, 0);
    sem_init(&p->sem_write, 0);

    p->readers = 1;
    p->writers = 1;

    *read_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    *write_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));

    if (!*read_node || !*write_node) {
        if (*read_node) kfree(*read_node);
        if (*write_node) kfree(*write_node);

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
    (*read_node)->flags = VFS_FLAG_PIPE_READ;
    (*read_node)->refs = 1u;

    strlcpy((*write_node)->name, "pipe_w", 32);
    (*write_node)->ops = &pipe_ops;
    (*write_node)->private_data = p;
    (*write_node)->private_retain = pipe_private_retain;
    (*write_node)->private_release = pipe_private_release;
    (*write_node)->flags = VFS_FLAG_PIPE_WRITE;
    (*write_node)->refs = 1u;

    poll_waitq_init_finalizable(&p->poll_waitq, pipe_poll_waitq_finalize, p);

    return 0;
}
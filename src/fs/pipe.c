/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <hal/lock.h>
#include <kernel/waitq/poll_waitq.h>
#include <kernel/sched.h>
#include <kernel/uaccess/uaccess.h>
#include <lib/string.h>
#include <mm/heap.h>

#include "pipe.h"

#define PIPE_SIZE 32768

/*
 * Pipe core.
 *
 * This is a byte-stream pipe implemented as a fixed-size ring buffer.
 *
 * Concurrency model:
 *  - `lock` protects the ring indices and the reader/writer counters.
 *  - I/O is coordinated via semaphores:
 *      sem_read  == number of readable bytes
 *      sem_write == number of free bytes
 *
 * Invariants:
 *  - write_ptr and read_ptr are monotonic counters; the ring index is derived
 *    via modulo.
 *  - write_ptr - read_ptr is the current occupancy and is bounded by `size`.
 *  - operations must re-check reader/writer counters after blocking, because
 *    close can happen concurrently.
 */

typedef struct {
    char* buffer;
    uint32_t size;
    uint32_t read_ptr;
    uint32_t write_ptr;

    semaphore_t sem_read;
    semaphore_t sem_write;

    poll_waitq_t poll_waitq;

    int readers;
    int writers;

    uint32_t refs;

    spinlock_t lock;
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

    __sync_fetch_and_add(&p->refs, 1u);
}

static void pipe_private_release(void* private_data) {
    pipe_t* p = (pipe_t*)private_data;
    if (!p) {
        return;
    }

    if (__sync_sub_and_fetch(&p->refs, 1u) != 0u) {
        return;
    }

    poll_waitq_detach_all(&p->poll_waitq);

    poll_waitq_put(&p->poll_waitq);
}

/*
 * Semaphore helpers.
 *
 * The pipe uses the semaphore as a counting resource rather than a simple
 * binary wakeup. We therefore frequently need "take up to N" and "signal N"
 * patterns.
 */

static uint32_t sem_take_up_to(semaphore_t* sem, uint32_t max) {
    if (max == 0) {
        return 0;
    }

    sem_wait(sem);

    uint32_t taken = 1;
    while (taken < max
           && sem_try_acquire(sem)) {
        taken++;
    }

    return taken;
}

static uint32_t sem_try_take_up_to(semaphore_t* sem, uint32_t max) {
    if (!sem
        || max == 0) {
        return 0;
    }

    /*
     * Peek and decrement the semaphore count without sleeping.
     * This is intentionally implemented with the internal semaphore lock.
     */
    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    int c = sem->count;
    if (c <= 0) {
        spinlock_release_safe(&sem->lock, flags);
        return 0;
    }

    uint32_t avail = (uint32_t)c;
    uint32_t take = (avail < max) ? avail : max;

    sem->count -= (int)take;
    spinlock_release_safe(&sem->lock, flags);

    return take;
}

static void sem_signal_n(semaphore_t* sem, uint32_t n) {
    if (!sem
        || n == 0) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    sem->count += (int)n;

    while (n--
           && !dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        __sync_fetch_and_add(&t->in_transit, 1);

        dlist_del(&t->sem_node);

        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;
        t->blocked_kind = TASK_BLOCK_NONE;

        if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
            __sync_fetch_and_sub(&t->in_transit, 1);
            continue;
        }

        if (proc_change_state(t, TASK_RUNNABLE) == 0) {
            sched_add(t);
        }

        __sync_fetch_and_sub(&t->in_transit, 1);
    }

    spinlock_release_safe(&sem->lock, flags);
}

static int sem_try_take_n(semaphore_t* sem, uint32_t n) {
    if (n == 0) {
        return 1;
    }

    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    if (sem->count >= (int)n) {
        sem->count -= (int)n;
        spinlock_release_safe(&sem->lock, flags);
        return 1;
    }
    spinlock_release_safe(&sem->lock, flags);
    return 0;
}

static void sem_wake_all(semaphore_t* sem) {
    uint32_t flags = spinlock_acquire_safe(&sem->lock);

    while (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        __sync_fetch_and_add(&t->in_transit, 1);

        dlist_del(&t->sem_node);

        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;
        t->blocked_kind = TASK_BLOCK_NONE;

        sem->count++;

        if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
            __sync_fetch_and_sub(&t->in_transit, 1);
            continue;
        }

        if (proc_change_state(t, TASK_RUNNABLE) == 0) {
            sched_add(t);
        }

        __sync_fetch_and_sub(&t->in_transit, 1);
    }

    spinlock_release_safe(&sem->lock, flags);
}

static int pipe_read(
    vfs_node_t* node,
    uint32_t offset,
    uint32_t size,
    void* buffer
) {
    (void)offset;

    /*
     * Blocking read semantics.
     *
     * The pipe is a byte stream: reads are satisfied from the ring buffer.
     * If there is no data available:
     *  - return 0/short read only on EOF (no writers)
     *  - otherwise, block waiting for sem_read.
     */

    pipe_t* p = (pipe_t*)node->private_data;
    char* buf = (char*)buffer;
    uint32_t read_count = 0;

    while (read_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);

        uint32_t available = p->write_ptr - p->read_ptr;
        int writers = p->writers;

        spinlock_release_safe(&p->lock, flags);

        if (available == 0
            && writers == 0) {
            return (int)read_count;
        }

        uint32_t want = size - read_count;
        uint32_t take = sem_take_up_to(&p->sem_read, want);

        flags = spinlock_acquire_safe(&p->lock);

        uint32_t now_avail = p->write_ptr - p->read_ptr;

        if (now_avail == 0
            && p->writers == 0) {
            spinlock_release_safe(&p->lock, flags);

            sem_signal_n(&p->sem_read, take);

            return (int)read_count;
        }

        /*
         * `take` is only an accounting step against sem_read.
         * We still must clamp to the actual ring occupancy.
         */
        uint32_t n = take;
        if (n > now_avail) {
            n = now_avail;
        }

        /* Split copy at wrap boundary. */
        uint32_t rp = p->read_ptr % p->size;
        uint32_t contig = p->size - rp;

        uint32_t n1 = n;
        if (n1 > contig) {
            n1 = contig;
        }

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

        /* Return any over-taken credits back to sem_read. */
        if (n < take) {
            sem_signal_n(&p->sem_read, take - n);
        }

        /* Each consumed byte becomes free space. */
        sem_signal_n(&p->sem_write, n);
        if (n > 0) {
            /* Wake poll waiters for both read and write side state changes. */
            poll_waitq_wake_all(&p->poll_waitq);
        }

        /*
         * Keep reads responsive: once we made progress, return a short read
         * instead of trying to fill the entire request.
         */
        if (read_count > 0) {
            return (int)read_count;
        }
    }

    return (int)read_count;
}

int pipe_read_nonblock(
    vfs_node_t* node,
    uint32_t size,
    void* buffer
) {
    /*
     * Non-blocking read.
     *
     * This path never sleeps. It opportunistically consumes up to `size` bytes
     * if sem_read indicates availability.
     */
    if (!node
        || !buffer
        || size == 0) {
        return 0;
    }

    if ((node->flags & VFS_FLAG_PIPE_READ) == 0) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;
    char* buf = (char*)buffer;

    uint32_t flags = spinlock_acquire_safe(&p->lock);

    uint32_t available = p->write_ptr - p->read_ptr;
    int writers = p->writers;

    spinlock_release_safe(&p->lock, flags);

    /* If empty, either EOF (no writers) or would-block. */
    if (available == 0) {
        if (writers == 0) {
            return -1;
        }
        return 0;
    }

    uint32_t want = size;
    uint32_t take = sem_try_take_up_to(&p->sem_read, want);
    if (take == 0) {
        return 0;
    }

    flags = spinlock_acquire_safe(&p->lock);

    uint32_t now_avail = p->write_ptr - p->read_ptr;

    if (now_avail == 0
        && p->writers == 0) {
        spinlock_release_safe(&p->lock, flags);

        sem_signal_n(&p->sem_read, take);

        return -1;
    }

    /* Clamp to actual occupancy after taking credits. */
    uint32_t n = take;
    if (n > now_avail) {
        n = now_avail;
    }

    /* Split copy at wrap boundary. */
    uint32_t rp = p->read_ptr % p->size;
    uint32_t contig = p->size - rp;

    uint32_t n1 = n;
    if (n1 > contig) {
        n1 = contig;
    }

    if (uaccess_copy_to_user(&buf[0], &p->buffer[rp], n1) != 0) {
        spinlock_release_safe(&p->lock, flags);

        sem_signal_n(&p->sem_read, take);

        return -1;
    }

    p->read_ptr += n1;

    uint32_t n2 = n - n1;
    if (n2 > 0) {
        if (uaccess_copy_to_user(&buf[n1], &p->buffer[0], n2) != 0) {
            p->read_ptr -= n1;

            spinlock_release_safe(&p->lock, flags);

            sem_signal_n(&p->sem_read, take);

            return -1;
        }

        p->read_ptr += n2;
    }

    spinlock_release_safe(&p->lock, flags);

    /* Return any over-taken credits back to sem_read and wake waiters. */
    if (n < take) {
        sem_signal_n(&p->sem_read, take - n);
    }

    sem_signal_n(&p->sem_write, n);
    if (n > 0) {
        poll_waitq_wake_all(&p->poll_waitq);
    }

    return (int)n;
}

int pipe_write_nonblock(
    vfs_node_t* node,
    uint32_t size,
    const void* buffer
) {
    /*
     * Non-blocking write.
     *
     * If no readers exist, this is treated as a broken pipe. If there is not
     * enough space, the call returns immediately.
     */
    if (!node
        || !buffer
        || size == 0) {
        return 0;
    }

    if ((node->flags & VFS_FLAG_PIPE_WRITE) == 0) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;
    if (size > p->size) {
        /*
         * The non-blocking interface is all-or-nothing.
         * Requests larger than the ring capacity can never complete.
         */
        return 0;
    }

    const char* buf = (const char*)buffer;

    uint32_t flags = spinlock_acquire_safe(&p->lock);

    int readers = p->readers;

    spinlock_release_safe(&p->lock, flags);

    /* Broken pipe: writers fail immediately if there are no readers. */
    if (readers == 0) {
        return -1;
    }

    /* Require full space or return would-block. */
    if (!sem_try_take_n(&p->sem_write, size)) {
        return 0;
    }

    flags = spinlock_acquire_safe(&p->lock);

    if (p->readers == 0) {
        spinlock_release_safe(&p->lock, flags);

        sem_signal_n(&p->sem_write, size);

        return -1;
    }

    /* Split copy at wrap boundary. */
    uint32_t wp = p->write_ptr % p->size;
    uint32_t contig = p->size - wp;

    uint32_t n1 = size;
    if (n1 > contig) {
        n1 = contig;
    }

    if (uaccess_copy_from_user(&p->buffer[wp], &buf[0], n1) != 0) {
        spinlock_release_safe(&p->lock, flags);

        sem_signal_n(&p->sem_write, size);

        return -1;
    }

    p->write_ptr += n1;

    uint32_t n2 = size - n1;
    if (n2 > 0) {
        if (uaccess_copy_from_user(&p->buffer[0], &buf[n1], n2) != 0) {
            p->write_ptr -= n1;

            spinlock_release_safe(&p->lock, flags);

            sem_signal_n(&p->sem_write, size);

            return -1;
        }

        p->write_ptr += n2;
    }

    spinlock_release_safe(&p->lock, flags);

    sem_signal_n(&p->sem_read, size);
    poll_waitq_wake_all(&p->poll_waitq);

    return (int)size;
}

static int pipe_write(
    vfs_node_t* node,
    uint32_t offset,
    uint32_t size,
    const void* buffer
) {
    (void)offset;

    /*
     * Blocking write semantics.
     *
     * The caller writes a byte stream into the ring buffer.
     * If no readers exist, the write fails with -1 (broken pipe).
     * Otherwise, the writer blocks on sem_write for space.
     */

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
        uint32_t take = sem_take_up_to(&p->sem_write, want);

        flags = spinlock_acquire_safe(&p->lock);

        if (p->readers == 0) {
            spinlock_release_safe(&p->lock, flags);

            sem_signal_n(&p->sem_write, take);

            return (written_count > 0) ? (int)written_count : -1;
        }

        /* `take` is a credit reservation; clamp is still required. */
        uint32_t n = take;
        uint32_t wp = p->write_ptr % p->size;
        uint32_t contig = p->size - wp;

        uint32_t n1 = n;
        if (n1 > contig) {
            n1 = contig;
        }

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

        /* Each written byte becomes readable data. */
        sem_signal_n(&p->sem_read, n);
        if (n > 0) {
            poll_waitq_wake_all(&p->poll_waitq);
        }
    }

    return (int)written_count;
}

int pipe_poll_waitq_register(
    vfs_node_t* node,
    poll_waiter_t* w,
    task_t* task
) {
    /*
     * Poll registration is delegated to the pipe's waitqueue.
     * Callers are expected to query pipe_poll_info() and then register.
     */
    if (!node
        || !w
        || !task) {
        return -1;
    }

    if ((node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) == 0) {
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

    if ((node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) == 0) {
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
        if ((events & VFS_POLLIN) && avail > 0) {
            rev |= VFS_POLLIN;
        }
        if (writers == 0) {
            rev |= VFS_POLLHUP;
        }
    }

    if (node->flags & VFS_FLAG_PIPE_WRITE) {
        if ((events & VFS_POLLOUT) && readers > 0 && space > 0) {
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

int pipe_poll_info(
    vfs_node_t* node,
    uint32_t* out_available,
    uint32_t* out_space,
    int* out_readers,
    int* out_writers
) {
    /*
     * Poll snapshot.
     *
     * The computed values are derived from monotonic indices, so they are
     * consistent as long as the snapshot is taken under the pipe lock.
     */
    if (out_available) {
        *out_available = 0;
    }
    if (out_space) {
        *out_space = 0;
    }
    if (out_readers) {
        *out_readers = 0;
    }
    if (out_writers) {
        *out_writers = 0;
    }

    if (!node) {
        return -1;
    }

    if ((node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) == 0) {
        return -1;
    }

    pipe_t* p = (pipe_t*)node->private_data;
    if (!p) {
        return -1;
    }

    uint32_t flags = spinlock_acquire_safe(&p->lock);

    /*
     * Snapshot ring occupancy first. `space` is derived from it so both values
     * remain consistent for this call.
     */
    uint32_t available = p->write_ptr - p->read_ptr;
    uint32_t space = p->size - available;

    int readers = p->readers;
    int writers = p->writers;

    spinlock_release_safe(&p->lock, flags);

    if (out_available) {
        *out_available = available;
    }
    if (out_space) {
        *out_space = space;
    }
    if (out_readers) {
        *out_readers = readers;
    }
    if (out_writers) {
        *out_writers = writers;
    }

    return 0;
}

static int pipe_close(vfs_node_t* node) {
    pipe_t* p = (pipe_t*)node->private_data;

    /*
     * Close semantics.
     *
     * Reader/writer counters are used to decide when to tear the pipe down.
     * When the last endpoint closes, the pipe is freed.
     *
     * Waiting operations are force-unblocked by waking both semaphores.
     */

    uint32_t flags = spinlock_acquire_safe(&p->lock);

    if (node->flags & VFS_FLAG_PIPE_READ) {
        p->readers--;
    }
    else if (node->flags & VFS_FLAG_PIPE_WRITE) {
        p->writers--;
    }

    int readers = p->readers;
    int writers = p->writers;

    spinlock_release_safe(&p->lock, flags);

    sem_wake_all(&p->sem_read);
    sem_wake_all(&p->sem_write);

    /* Unblock pollers even if the pipe becomes dead immediately. */
    poll_waitq_wake_all(&p->poll_waitq);

    if (readers == 0
        && writers == 0) {
        poll_waitq_detach_all(&p->poll_waitq);
    }
    return 0;
}

static vfs_ops_t pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
    .open = 0,
    .ioctl = 0,
    .get_phys_page = 0,
    .poll_status = pipe_vfs_poll_status,
    .poll_register = pipe_vfs_poll_register,
};

int vfs_create_pipe(
    vfs_node_t** read_node,
    vfs_node_t** write_node
) {
    /*
     * Create paired read/write VFS nodes sharing one pipe backend.
     * The backend is freed when the last endpoint is closed.
     */
    if (read_node) {
        *read_node = 0;
    }
    if (write_node) {
        *write_node = 0;
    }

    if (!read_node
        || !write_node) {
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

    spinlock_init(&p->lock);

    sem_init(&p->sem_read, 0);
    sem_init(&p->sem_write, p->size);

    /* One initial reader and writer: each endpoint consumes one reference. */
    p->readers = 1;
    p->writers = 1;

    *read_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    *write_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));

    if (!*read_node
        || !*write_node) {
        if (*read_node) {
            kfree(*read_node);
            *read_node = 0;
        }
        if (*write_node) {
            kfree(*write_node);
            *write_node = 0;
        }

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

    strlcpy((*write_node)->name, "pipe_w", 32);
    (*write_node)->ops = &pipe_ops;
    (*write_node)->private_data = p;
    (*write_node)->private_retain = pipe_private_retain;
    (*write_node)->private_release = pipe_private_release;
    (*write_node)->inode_idx = 0;
    (*write_node)->size = 0;
    (*write_node)->flags = VFS_FLAG_PIPE_WRITE;

    (*read_node)->refs = 1;
    (*write_node)->refs = 1;

    poll_waitq_init_finalizable(&p->poll_waitq, pipe_poll_waitq_finalize, p);

    return 0;
}

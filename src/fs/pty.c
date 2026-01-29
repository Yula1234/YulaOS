// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <hal/lock.h>
#include <kernel/sched.h>
#include <kernel/poll_waitq.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <yos/ioctl.h>

#include "pty.h"

#define PTY_BUF_SIZE 4096u
#define PTY_BATCH    1024u

typedef struct {
    char buffer[PTY_BUF_SIZE];
    uint32_t read_ptr;
    uint32_t write_ptr;

    semaphore_t sem_read;
    semaphore_t sem_write;
} pty_chan_t;

typedef struct {
    volatile uint32_t refs;

    spinlock_t lock;
    poll_waitq_t poll_waitq;

    pty_chan_t m2s;
    pty_chan_t s2m;

    uint32_t id;
    int devfs_registered;

    vfs_node_t* slave_node;

    int master_open;
    int slave_open;

    yos_termios_t termios;
    yos_winsize_t winsz;
} pty_pair_t;

static spinlock_t pty_id_lock;
static uint32_t pty_next_id = 1u;

static uint32_t pty_alloc_id(void) {
    uint32_t flags = spinlock_acquire_safe(&pty_id_lock);
    uint32_t id = pty_next_id++;
    if (pty_next_id == 0u) pty_next_id = 1u;
    spinlock_release_safe(&pty_id_lock, flags);
    return id;
}

static void pty_make_pts_name(char out[32], uint32_t id) {
    char tmp[11];
    uint32_t n = 0;

    if (id == 0u) {
        tmp[n++] = '0';
    } else {
        while (id > 0u && n < 10u) {
            tmp[n++] = (char)('0' + (id % 10u));
            id /= 10u;
        }
    }

    uint32_t pos = 0;
    out[pos++] = 'p';
    out[pos++] = 't';
    out[pos++] = 's';
    out[pos++] = '/';

    while (n > 0u && pos < 31u) {
        out[pos++] = tmp[--n];
    }
    out[pos] = '\0';
}

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

static uint32_t sem_try_take_up_to(semaphore_t* sem, uint32_t max) {
    if (!sem || max == 0) return 0;
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
    if (!sem || n == 0) return;

    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    sem->count += (int)n;

    while (n-- && !dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);
        dlist_del(&t->sem_node);
        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;
        if (t->state != TASK_ZOMBIE) {
            t->state = TASK_RUNNABLE;
            sched_add(t);
        }
    }

    spinlock_release_safe(&sem->lock, flags);
}

static int sem_try_take_n(semaphore_t* sem, uint32_t n) {
    if (n == 0) return 1;
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

        dlist_del(&t->sem_node);
        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;

        sem->count++;

        if (t->state != TASK_ZOMBIE) {
            t->state = TASK_RUNNABLE;
            sched_add(t);
        }
    }

    spinlock_release_safe(&sem->lock, flags);
}

static void pty_pair_retain(void* private_data) {
    if (!private_data) return;
    pty_pair_t* p = (pty_pair_t*)private_data;
    __sync_fetch_and_add(&p->refs, 1);
}

static void pty_pair_release(void* private_data) {
    if (!private_data) return;
    pty_pair_t* p = (pty_pair_t*)private_data;
    if (__sync_sub_and_fetch(&p->refs, 1) != 0) {
        return;
    }

    poll_waitq_detach_all(&p->poll_waitq);
    kfree(p);
}

static pty_pair_t* pty_pair_create(void) {
    pty_pair_t* p = (pty_pair_t*)kmalloc(sizeof(*p));
    if (!p) return 0;

    memset(p, 0, sizeof(*p));

    p->refs = 1;
    spinlock_init(&p->lock);
    poll_waitq_init(&p->poll_waitq);

    sem_init(&p->m2s.sem_read, 0);
    sem_init(&p->m2s.sem_write, (int)PTY_BUF_SIZE);

    sem_init(&p->s2m.sem_read, 0);
    sem_init(&p->s2m.sem_write, (int)PTY_BUF_SIZE);

    p->winsz.ws_row = 25;
    p->winsz.ws_col = 80;
    p->winsz.ws_xpixel = 0;
    p->winsz.ws_ypixel = 0;

    return p;
}

static uint32_t pty_chan_read_locked(pty_chan_t* ch, char* dst, uint32_t n) {
    if (!ch || !dst || n == 0) return 0;

    uint32_t rp = ch->read_ptr % PTY_BUF_SIZE;
    uint32_t contig = PTY_BUF_SIZE - rp;

    uint32_t n1 = n;
    if (n1 > contig) n1 = contig;
    memcpy(&dst[0], &ch->buffer[rp], n1);
    ch->read_ptr += n1;

    uint32_t n2 = n - n1;
    if (n2 > 0) {
        memcpy(&dst[n1], &ch->buffer[0], n2);
        ch->read_ptr += n2;
    }

    return n;
}

static uint32_t pty_chan_write_locked(pty_chan_t* ch, const char* src, uint32_t n) {
    if (!ch || !src || n == 0) return 0;

    uint32_t wp = ch->write_ptr % PTY_BUF_SIZE;
    uint32_t contig = PTY_BUF_SIZE - wp;

    uint32_t n1 = n;
    if (n1 > contig) n1 = contig;
    memcpy(&ch->buffer[wp], &src[0], n1);
    ch->write_ptr += n1;

    uint32_t n2 = n - n1;
    if (n2 > 0) {
        memcpy(&ch->buffer[0], &src[n1], n2);
        ch->write_ptr += n2;
    }

    return n;
}

static int pty_chan_read(pty_pair_t* p, pty_chan_t* ch, uint32_t size, void* buffer, const int* peer_open_field) {
    if (!p || !ch || !buffer || size == 0) return 0;

    char* buf = (char*)buffer;
    uint32_t read_count = 0;

    while (read_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);
        uint32_t available = ch->write_ptr - ch->read_ptr;
        int peer_open = peer_open_field ? *peer_open_field : 0;
        spinlock_release_safe(&p->lock, flags);

        if (available == 0 && peer_open == 0) {
            return (int)read_count;
        }

        uint32_t want = size - read_count;
        if (want > PTY_BATCH) want = PTY_BATCH;

        uint32_t take = sem_take_up_to(&ch->sem_read, want);

        flags = spinlock_acquire_safe(&p->lock);
        uint32_t now_avail = ch->write_ptr - ch->read_ptr;
        peer_open = peer_open_field ? *peer_open_field : 0;

        if (now_avail == 0 && peer_open == 0) {
            spinlock_release_safe(&p->lock, flags);
            sem_give_n(&ch->sem_read, take);
            return (int)read_count;
        }

        uint32_t n = take;
        if (n > now_avail) n = now_avail;

        (void)pty_chan_read_locked(ch, &buf[read_count], n);
        read_count += n;

        spinlock_release_safe(&p->lock, flags);

        if (n < take) {
            sem_give_n(&ch->sem_read, take - n);
        }
        sem_give_n(&ch->sem_write, n);
        if (n > 0) {
            poll_waitq_wake_all(&p->poll_waitq);
        }
    }

    return (int)read_count;
}

static int pty_chan_read_nonblock(pty_pair_t* p, pty_chan_t* ch, uint32_t size, void* buffer, const int* peer_open_field) {
    if (!p || !ch || !buffer || size == 0) return 0;

    char* buf = (char*)buffer;

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    uint32_t available = ch->write_ptr - ch->read_ptr;
    int peer_open = peer_open_field ? *peer_open_field : 0;
    spinlock_release_safe(&p->lock, flags);

    if (available == 0) {
        if (peer_open == 0) return -1;
        return 0;
    }

    uint32_t want = size;
    if (want > PTY_BATCH) want = PTY_BATCH;

    uint32_t take = sem_try_take_up_to(&ch->sem_read, want);
    if (take == 0) return 0;

    flags = spinlock_acquire_safe(&p->lock);
    uint32_t now_avail = ch->write_ptr - ch->read_ptr;
    peer_open = peer_open_field ? *peer_open_field : 0;

    if (now_avail == 0 && peer_open == 0) {
        spinlock_release_safe(&p->lock, flags);
        sem_give_n(&ch->sem_read, take);
        return -1;
    }

    uint32_t n = take;
    if (n > now_avail) n = now_avail;

    (void)pty_chan_read_locked(ch, &buf[0], n);
    spinlock_release_safe(&p->lock, flags);

    if (n < take) {
        sem_signal_n(&ch->sem_read, take - n);
    }
    sem_signal_n(&ch->sem_write, n);
    if (n > 0) {
        poll_waitq_wake_all(&p->poll_waitq);
    }
    return (int)n;
}

static int pty_chan_write_nonblock(pty_pair_t* p, pty_chan_t* ch, uint32_t size, const void* buffer, const int* peer_open_field) {
    if (!p || !ch || !buffer || size == 0) return 0;
    if (size > PTY_BUF_SIZE) return 0;

    const char* buf = (const char*)buffer;

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    int peer_open = peer_open_field ? *peer_open_field : 0;
    spinlock_release_safe(&p->lock, flags);
    if (peer_open == 0) return -1;

    if (!sem_try_take_n(&ch->sem_write, size)) {
        return 0;
    }

    flags = spinlock_acquire_safe(&p->lock);
    peer_open = peer_open_field ? *peer_open_field : 0;
    if (peer_open == 0) {
        spinlock_release_safe(&p->lock, flags);
        sem_signal_n(&ch->sem_write, size);
        return -1;
    }

    (void)pty_chan_write_locked(ch, buf, size);
    spinlock_release_safe(&p->lock, flags);

    sem_signal_n(&ch->sem_read, size);
    poll_waitq_wake_all(&p->poll_waitq);
    return (int)size;
}

static int pty_chan_write(pty_pair_t* p, pty_chan_t* ch, uint32_t size, const void* buffer, const int* peer_open_field) {
    if (!p || !ch || !buffer || size == 0) return 0;

    const char* buf = (const char*)buffer;
    uint32_t written_count = 0;

    while (written_count < size) {
        uint32_t flags = spinlock_acquire_safe(&p->lock);
        int peer_open = peer_open_field ? *peer_open_field : 0;
        spinlock_release_safe(&p->lock, flags);

        if (peer_open == 0) {
            return (written_count > 0) ? (int)written_count : -1;
        }

        uint32_t want = size - written_count;
        if (want > PTY_BATCH) want = PTY_BATCH;

        uint32_t take = sem_take_up_to(&ch->sem_write, want);

        flags = spinlock_acquire_safe(&p->lock);
        peer_open = peer_open_field ? *peer_open_field : 0;
        if (peer_open == 0) {
            spinlock_release_safe(&p->lock, flags);
            sem_give_n(&ch->sem_write, take);
            return (written_count > 0) ? (int)written_count : -1;
        }

        uint32_t n = take;
        (void)pty_chan_write_locked(ch, &buf[written_count], n);
        written_count += n;

        spinlock_release_safe(&p->lock, flags);

        sem_give_n(&ch->sem_read, n);
        if (n > 0) {
            poll_waitq_wake_all(&p->poll_waitq);
        }
    }

    return (int)written_count;
}

static int pty_master_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;
    if (!node || !buffer || size == 0) return 0;
    if ((node->flags & VFS_FLAG_PTY_MASTER) == 0) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;
    return pty_chan_read(p, &p->s2m, size, buffer, &p->devfs_registered);
}

static int pty_master_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;
    if (!node || !buffer || size == 0) return 0;
    if ((node->flags & VFS_FLAG_PTY_MASTER) == 0) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;
    return pty_chan_write(p, &p->m2s, size, buffer, &p->devfs_registered);
}

static int pty_slave_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;
    if (!node || !buffer || size == 0) return 0;
    if ((node->flags & VFS_FLAG_PTY_SLAVE) == 0) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;
    return pty_chan_read(p, &p->m2s, size, buffer, &p->master_open);
}

static int pty_slave_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;
    if (!node || !buffer || size == 0) return 0;
    if ((node->flags & VFS_FLAG_PTY_SLAVE) == 0) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;
    return pty_chan_write(p, &p->s2m, size, buffer, &p->master_open);
}

static int pty_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    if (!node || !arg) return -1;
    if ((node->flags & (VFS_FLAG_PTY_MASTER | VFS_FLAG_PTY_SLAVE)) == 0) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    switch (req) {
        case YOS_TCGETS:
            memcpy(arg, &p->termios, sizeof(p->termios));
            break;
        case YOS_TCSETS:
            memcpy(&p->termios, arg, sizeof(p->termios));
            break;
        case YOS_TIOCGWINSZ:
            memcpy(arg, &p->winsz, sizeof(p->winsz));
            break;
        case YOS_TIOCSWINSZ:
            memcpy(&p->winsz, arg, sizeof(p->winsz));
            break;
        default:
            spinlock_release_safe(&p->lock, flags);
            return -1;
    }
    spinlock_release_safe(&p->lock, flags);
    return 0;
}

static int pty_master_open(vfs_node_t* node) {
    if (!node) return -1;
    if ((node->flags & VFS_FLAG_PTY_MASTER) == 0) return -1;
    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    p->master_open++;
    spinlock_release_safe(&p->lock, flags);
    return 0;
}

static int pty_slave_open(vfs_node_t* node) {
    if (!node) return -1;
    if ((node->flags & VFS_FLAG_PTY_SLAVE) == 0) return -1;
    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    if (p->master_open <= 0) {
        spinlock_release_safe(&p->lock, flags);
        return -1;
    }
    p->slave_open++;
    spinlock_release_safe(&p->lock, flags);
    return 0;
}

static int pty_close(vfs_node_t* node) {
    if (!node) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) {
        kfree(node);
        return 0;
    }

    int do_unregister = 0;
    uint32_t pts_id = 0;

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    if (node->flags & VFS_FLAG_PTY_MASTER) {
        if (p->master_open > 0) p->master_open--;
        if (p->master_open == 0 && p->devfs_registered) {
            do_unregister = 1;
            p->devfs_registered = 0;
            pts_id = p->id;
            p->slave_node = 0;
        }
    } else if (node->flags & VFS_FLAG_PTY_SLAVE) {
        if (p->slave_open > 0) p->slave_open--;
    }
    spinlock_release_safe(&p->lock, flags);

    sem_wake_all(&p->m2s.sem_read);
    sem_wake_all(&p->m2s.sem_write);
    sem_wake_all(&p->s2m.sem_read);
    sem_wake_all(&p->s2m.sem_write);
    poll_waitq_wake_all(&p->poll_waitq);

    if (do_unregister && pts_id != 0u) {
        char name[32];
        pty_make_pts_name(name, pts_id);
        vfs_node_t* tmpl = devfs_take(name);
        if (tmpl) {
            if (tmpl->private_release && tmpl->private_data) {
                tmpl->private_release(tmpl->private_data);
                tmpl->private_data = 0;
            }
            kfree(tmpl);
        } else {
            (void)devfs_unregister(name);
        }
    }

    if (node->private_release && node->private_data) {
        node->private_release(node->private_data);
        node->private_data = 0;
    }
    kfree(node);
    return 0;
}

static vfs_ops_t pty_master_ops = {
    .read = pty_master_read,
    .write = pty_master_write,
    .open = pty_master_open,
    .close = pty_close,
    .ioctl = pty_ioctl,
};

static vfs_ops_t pty_slave_ops = {
    .read = pty_slave_read,
    .write = pty_slave_write,
    .open = pty_slave_open,
    .close = pty_close,
    .ioctl = pty_ioctl,
};

static int pty_ptmx_open(vfs_node_t* node) {
    if (!node) return -1;

    pty_pair_t* p = pty_pair_create();
    if (!p) return -1;

    char pts_name[32];
    uint32_t id_flags = spinlock_acquire_safe(&pty_id_lock);
    for (;;) {
        uint32_t id = pty_next_id++;
        if (pty_next_id == 0u) pty_next_id = 1u;
        p->id = id;

        pty_make_pts_name(pts_name, p->id);
        if (!devfs_fetch(pts_name)) {
            break;
        }
    }

    node->flags |= VFS_FLAG_PTY_MASTER;
    node->ops = &pty_master_ops;
    node->private_data = p;
    node->private_retain = pty_pair_retain;
    node->private_release = pty_pair_release;

    if (pty_master_open(node) != 0) {
        spinlock_release_safe(&pty_id_lock, id_flags);
        return -1;
    }

    vfs_node_t* slave = (vfs_node_t*)kmalloc(sizeof(*slave));
    if (!slave) {
        spinlock_release_safe(&pty_id_lock, id_flags);
        return -1;
    }

    memset(slave, 0, sizeof(*slave));
    strlcpy(slave->name, pts_name, sizeof(slave->name));
    slave->flags = VFS_FLAG_PTY_SLAVE;
    slave->size = 0;
    slave->inode_idx = 0;
    slave->refs = 1;
    slave->ops = &pty_slave_ops;
    slave->private_data = p;
    slave->private_retain = pty_pair_retain;
    slave->private_release = pty_pair_release;

    pty_pair_retain(p);
    devfs_register(slave);
    if (devfs_fetch(pts_name) != slave) {
        pty_pair_release(p);
        kfree(slave);
        spinlock_release_safe(&pty_id_lock, id_flags);
        return -1;
    }

    spinlock_release_safe(&pty_id_lock, id_flags);

    uint32_t flags = spinlock_acquire_safe(&p->lock);
    p->slave_node = slave;
    p->devfs_registered = 1;
    spinlock_release_safe(&p->lock, flags);

    return 0;
}

void pty_init(void) {

    static vfs_ops_t ptmx_ops;
    static vfs_node_t ptmx_node;

    if (ptmx_node.name[0] != '\0') {
        return;
    }

    spinlock_init(&pty_id_lock);

    ptmx_ops.read = 0;
    ptmx_ops.write = 0;
    ptmx_ops.open = 0;
    ptmx_ops.close = 0;
    ptmx_ops.ioctl = 0;

    memset(&ptmx_node, 0, sizeof(ptmx_node));
    strlcpy(ptmx_node.name, "ptmx", sizeof(ptmx_node.name));
    ptmx_node.flags = 0;
    ptmx_node.size = 0;
    ptmx_node.inode_idx = 0;
    ptmx_node.refs = 1;
    ptmx_node.ops = &ptmx_ops;
    ptmx_node.private_data = 0;
    ptmx_node.private_retain = 0;
    ptmx_node.private_release = 0;

    ptmx_ops.open = pty_ptmx_open;

    devfs_register(&ptmx_node);
}

int pty_poll_info(vfs_node_t* node, uint32_t* out_available, uint32_t* out_space, int* out_peer_open) {
    if (out_available) *out_available = 0;
    if (out_space) *out_space = 0;
    if (out_peer_open) *out_peer_open = 0;

    if (!node) return -1;
    if ((node->flags & (VFS_FLAG_PTY_MASTER | VFS_FLAG_PTY_SLAVE)) == 0) return -1;

    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;

    uint32_t flags = spinlock_acquire_safe(&p->lock);

    uint32_t avail = 0;
    uint32_t space = 0;
    int peer_open = 0;

    if (node->flags & VFS_FLAG_PTY_MASTER) {
        avail = p->s2m.write_ptr - p->s2m.read_ptr;
        space = PTY_BUF_SIZE - (p->m2s.write_ptr - p->m2s.read_ptr);
        peer_open = p->devfs_registered;
    } else {
        avail = p->m2s.write_ptr - p->m2s.read_ptr;
        space = PTY_BUF_SIZE - (p->s2m.write_ptr - p->s2m.read_ptr);
        peer_open = p->master_open;
    }

    spinlock_release_safe(&p->lock, flags);

    if (out_available) *out_available = avail;
    if (out_space) *out_space = space;
    if (out_peer_open) *out_peer_open = peer_open;
    return 0;
}

int pty_poll_waitq_register(vfs_node_t* node, poll_waiter_t* w, struct task* task) {
    if (!node || !w || !task) return -1;
    if ((node->flags & (VFS_FLAG_PTY_MASTER | VFS_FLAG_PTY_SLAVE)) == 0) return -1;
    pty_pair_t* p = (pty_pair_t*)node->private_data;
    if (!p) return -1;
    return poll_waitq_register(&p->poll_waitq, w, task);
}

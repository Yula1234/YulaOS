/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/waitq/poll_waitq.h>
#include <kernel/locking/guards.h>
#include <kernel/tty/core.h>
#include <kernel/tty/pty.h>
#include <kernel/sched.h>
#include <kernel/panic.h>
#include <kernel/proc.h>

#include <lib/string.h>
#include <lib/idr.h>

#include <yos/ioctl.h>

#include <hal/lock.h>

#include <mm/heap.h>

typedef struct {
    uint32_t   id;

    spinlock_t lock;

    volatile uint32_t refs;

    int master_open;

    int slave_open;
    int slave_ever_opened;
    
    int devfs_registered;

    tty_t* master_tty;
    tty_t* slave_tty;
} pty_pair_t;

static idr_t g_pty_idr;

static void pty_make_pts_name(char out[32], uint32_t id) {
    char tmp[11];
    uint32_t n = 0u;

    if (id == 0u) {
        tmp[n++] = '0';
    } else {
        uint32_t val = id;
        
        while (val > 0u && n < 10u) {
            tmp[n++] = (char)('0' + (val % 10u));
            val /= 10u;
        }
    }

    uint32_t pos = 0u;
    
    out[pos++] = 'p';
    out[pos++] = 't';
    out[pos++] = 's';
    out[pos++] = '/';

    while (n > 0u && pos < 31u) {
        out[pos++] = tmp[--n];
    }
    
    out[pos] = '\0';
}

static void pty_pair_destroy(pty_pair_t* p) {
    if (unlikely(!p)) {
        return;
    }

    kfree(p);
}

static void pty_pair_retain(pty_pair_t* p) {
    if (unlikely(!p)) {
        return;
    }

    for (;;) {
        uint32_t expected = __atomic_load_n(&p->refs, __ATOMIC_RELAXED);
        
        if (unlikely(expected == 0u)) {
            panic("PTY: pair_retain after free");
        }

        if (__atomic_compare_exchange_n(
                &p->refs, &expected, expected + 1u,
                0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
            )) {
            return;
        }
    }
}

static void pty_pair_release(pty_pair_t* p) {
    if (unlikely(!p)) {
        return;
    }

    for (;;) {
        uint32_t expected = __atomic_load_n(&p->refs, __ATOMIC_RELAXED);
        
        if (unlikely(expected == 0u)) {
            panic("PTY: pair_release underflow");
        }

        const uint32_t desired = expected - 1u;

        if (__atomic_compare_exchange_n(
                &p->refs, &expected, desired,
                0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
            )) {
            
            if (desired == 0u) {
                pty_pair_destroy(p);
            }
            
            return;
        }
    }
}

static int pty_master_hw_write(tty_t* tty, const void* buf, uint32_t size) {
    if (unlikely(!tty
        || !buf
        || size == 0u)) {
        return 0;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;

    int is_slave_open = 0;

    {
        guard_spinlock_safe(&p->lock);
        
        is_slave_open = p->slave_open;
    }

    if (!is_slave_open) {
        return -1; 
    }

    if (likely(p->slave_tty)) {
        tty_receive(p->slave_tty, (const uint8_t*)buf, size);
    }

    return (int)size;
}

static void pty_master_hw_close(tty_t* tty) {
    if (unlikely(!tty)) {
        return;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;

    int do_unregister = 0;
    uint32_t pts_id = 0u;

    {
        guard_spinlock_safe(&p->lock);
        
        if (p->master_open > 0) {
            p->master_open--;
        }

        if (p->master_open == 0
            && p->devfs_registered) {
            do_unregister = 1;
            
            p->devfs_registered = 0;
            
            pts_id = p->id;
        }
    }

    if (p->slave_tty) {
        poll_waitq_wake_all(&p->slave_tty->poll_waitq, VFS_POLLHUP | VFS_POLLIN | VFS_POLLOUT);
    }

    if (do_unregister
        && pts_id != 0u) {
        idr_remove(&g_pty_idr, (int)pts_id);

        char name[32];
        pty_make_pts_name(name, pts_id);

        vfs_node_t* slave_node = devfs_take(name);
        
        if (slave_node) {
            vfs_node_release(slave_node);
        }
    }

    pty_pair_release(p);
}

static int pty_master_hw_poll_status(tty_t* tty, int events) {
    if (unlikely(!tty)) {
        return 0;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;
    
    int revents = 0;
    int slave_open = 0;

    {
        guard_spinlock_safe(&p->lock);
        
        slave_open = p->slave_open;
    }

    if ((events & VFS_POLLOUT) != 0) {
        if (slave_open > 0) {
            revents |= VFS_POLLOUT;
        }
    }

    if (p->slave_ever_opened && slave_open == 0) {
        revents |= VFS_POLLHUP;
    }

    return revents;
}

static int pty_master_hw_ioctl(tty_t* tty, uint32_t req, void* arg) {
    if (unlikely(!tty)) {
        return -1;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;

    switch (req) {
        case YOS_TIOCGPTN: {
            if (!arg) return -1;

            *(uint32_t*)arg = p->id;
            return 0;
        }
        default:
            return -1;
    }
}

static const tty_driver_ops_t g_pty_master_ops = {
    .write         = pty_master_hw_write,
    .ioctl         = pty_master_hw_ioctl,
    .close         = pty_master_hw_close,
    .poll_status   = pty_master_hw_poll_status,
    .open = 0, .set_termios = 0,
};

static int pty_slave_hw_open(tty_t* tty) {
    if (unlikely(!tty)) {
        return -1;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;

    guard_spinlock_safe(&p->lock);
    
    p->slave_open++;
    p->slave_ever_opened = 1;
    
    return 0;
}

static int pty_slave_hw_write(tty_t* tty, const void* buf, uint32_t size) {
    if (unlikely(!tty
        || !buf
        || size == 0u)) {
        return 0;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;

    int is_master_open = 0;

    {
        guard_spinlock_safe(&p->lock);
        
        is_master_open = p->master_open;
    }

    if (!is_master_open) {
        return -1; 
    }

    if (likely(p->master_tty)) {
        tty_receive(p->master_tty, (const uint8_t*)buf, size);
    }

    return (int)size;
}

static void pty_slave_hw_close(tty_t* tty) {
    if (unlikely(!tty)) {
        return;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;

    {
        guard_spinlock_safe(&p->lock);
        
        if (p->slave_open > 0) {
            p->slave_open--;
        }
    }

    if (p->master_tty) {
        poll_waitq_wake_all(&p->master_tty->poll_waitq, VFS_POLLHUP | VFS_POLLIN | VFS_POLLOUT);
    }

    pty_pair_release(p);
}

static int pty_slave_hw_poll_status(tty_t* tty, int events) {
    if (unlikely(!tty)) {
        return 0;
    }

    pty_pair_t* p = (pty_pair_t*)tty->driver_data;
    
    int revents = 0;
    int master_open = 0;

    {
        guard_spinlock_safe(&p->lock);
        
        master_open = p->master_open;
    }

    if ((events & VFS_POLLOUT) != 0) {
        if (master_open > 0) {
            revents |= VFS_POLLOUT;
        }
    }

    if (master_open == 0) {
        revents |= VFS_POLLHUP;
    }

    return revents;
}

static const tty_driver_ops_t g_pty_slave_ops = {
    .open          = pty_slave_hw_open,
    .write         = pty_slave_hw_write,
    .close         = pty_slave_hw_close,
    .poll_status   = pty_slave_hw_poll_status,
    .ioctl = 0, .set_termios = 0,
};

static void configure_raw_ldisc(tty_t* tty) {
    if (!tty
        || !tty->ldisc) {
        return;
    }

    ldisc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.canonical_ = 0;
    cfg.echo_      = 0;
    cfg.isig_      = 0;
    cfg.igncr_     = 0;
    cfg.icrnl_     = 0;
    cfg.inlcr_     = 0;
    cfg.opost_     = 0;
    cfg.onlcr_     = 0;
    cfg.vmin_      = 1u;
    cfg.vtime_     = 0u;

    ldisc_set_config(tty->ldisc, &cfg);
}

static int pty_ptmx_open(vfs_node_t* node) {
    if (unlikely(!node)) {
        return -1;
    }

    pty_pair_t* p = (pty_pair_t*)kmalloc(sizeof(pty_pair_t));
    if (!p) {
        return -1;
    }

    memset(p, 0, sizeof(*p));

    p->refs = 1u;

    spinlock_init(&p->lock);

    int id = idr_alloc(&g_pty_idr, p);

    if (id < 0) {
        pty_pair_release(p);
        return -1;
    }

    p->id = (uint32_t)id;

    pty_pair_retain(p);
    
    p->master_tty = tty_alloc(&g_pty_master_ops, p);
    
    if (!p->master_tty) {
        pty_pair_release(p);
        idr_remove(&g_pty_idr, id);
        pty_pair_release(p);
        return -1;
    }
    
    configure_raw_ldisc(p->master_tty);

    pty_pair_retain(p);
    
    p->slave_tty = tty_alloc(&g_pty_slave_ops, p);
    
    if (!p->slave_tty) {
        pty_pair_release(p);
        tty_release(p->master_tty);
        idr_remove(&g_pty_idr, id);
        pty_pair_release(p);
        return -1;
    }

    node->flags |= VFS_FLAG_PTY_MASTER;
    
    tty_bind_vfs_node(p->master_tty, node);

    tty_release(p->master_tty);

    vfs_node_t* slave_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    
    if (!slave_node) {
        tty_release(p->slave_tty);
        tty_release(p->master_tty);
        idr_remove(&g_pty_idr, id);
        pty_pair_release(p);
        return -1;
    }

    memset(slave_node, 0, sizeof(*slave_node));
    pty_make_pts_name(slave_node->name, p->id);

    slave_node->flags = VFS_FLAG_PTY_SLAVE;
    slave_node->refs  = 1u;

    tty_bind_vfs_node(p->slave_tty, slave_node);

    tty_release(p->slave_tty);

    devfs_register(slave_node);

    {
        guard_spinlock_safe(&p->lock);
        
        p->master_open = 1;
        p->devfs_registered = 1;
    }

    pty_pair_release(p);

    return 0;
}

void pty_init(void) {
    static vfs_ops_t ptmx_ops;
    static vfs_node_t ptmx_node;

    if (ptmx_node.name[0] != '\0') {
        return;
    }

    idr_init(&g_pty_idr);

    memset(&ptmx_ops, 0, sizeof(ptmx_ops));
    ptmx_ops.open = pty_ptmx_open;

    memset(&ptmx_node, 0, sizeof(ptmx_node));
    strlcpy(ptmx_node.name, "ptmx", sizeof(ptmx_node.name));

    ptmx_node.refs = 1u;
    ptmx_node.ops  = &ptmx_ops;

    devfs_register(&ptmx_node);
}
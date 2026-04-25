/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <kernel/waitq/poll_waitq.h>
#include <kernel/locking/guards.h>
#include <kernel/tty/core.h>
#include <kernel/sched.h>
#include <kernel/panic.h>
#include <kernel/proc.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <mm/heap.h>


static int tty_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer);
static int tty_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer);

static int tty_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg);

static int tty_vfs_open(vfs_node_t* node);

static int tty_vfs_poll_status(vfs_node_t* node, int events);
static int tty_vfs_poll_register(vfs_node_t* node, poll_waiter_t* w, task_t* task);

static vfs_ops_t g_tty_core_vfs_ops = {
    .read          = tty_vfs_read,
    .write         = tty_vfs_write,
    .open          = tty_vfs_open,
    .ioctl         = tty_vfs_ioctl,
    .poll_status   = tty_vfs_poll_status,
    .poll_register = tty_vfs_poll_register,
    .get_phys_page = 0, .close = 0,
};

static int tty_vfs_open(vfs_node_t* node) {
    if (unlikely(!node)) {
        return -1;
    }

    tty_t* tty = (tty_t*)node->private_data;
    
    if (tty && tty->ops && tty->ops->open) {
        return tty->ops->open(tty);
    }
    return 0;
}

static size_t tty_echo_callback(const uint8_t* data, size_t size, void* ctx) {
    if (unlikely(!data
        || size == 0u
        || !ctx)) {
        return 0u;
    }

    tty_t* tty = (tty_t*)ctx;

    if (tty->ops && tty->ops->write) {
        int written = tty->ops->write(tty, data, size);

        return (written > 0) ? (size_t)written : 0u;
    }

    return size;
}

static void tty_signal_callback(int sig, void* ctx) {
    if (unlikely(!ctx)) {
        return;
    }

    tty_t* tty = (tty_t*)ctx;

    uint32_t pgid = 0u;

    {
        guard_spinlock_safe(&tty->lock);
        
        pgid = tty->fg_pgid;
    }

    if (pgid == 0u) {
        task_t* curr = proc_current();

        if (curr) {
            pgid = curr->pgid;
        }
    }

    if (pgid != 0u) {
        (void)proc_signal_pgrp(pgid, (uint32_t)sig);
    }
}

static void tty_poll_waitq_finalize(void* ctx) {
    tty_t* tty = (tty_t*)ctx;

    if (unlikely(!tty)) {
        return;
    }

    kfree(tty);
}

static ldisc_config_t config_from_termios(const yos_termios_t* t) {
    ldisc_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));

    cfg.canonical_ = (t->c_lflag & YOS_LFLAG_ICANON) != 0;
    cfg.echo_      = (t->c_lflag & YOS_LFLAG_ECHO)   != 0;
    cfg.isig_      = (t->c_lflag & YOS_LFLAG_ISIG)   != 0;

    cfg.igncr_     = (t->c_iflag & YOS_IFLAG_IGNCR)  != 0;
    cfg.icrnl_     = (t->c_iflag & YOS_IFLAG_ICRNL)  != 0;
    cfg.inlcr_     = (t->c_iflag & YOS_IFLAG_INLCR)  != 0;

    cfg.opost_     = (t->c_oflag & YOS_OFLAG_OPOST)  != 0;
    cfg.onlcr_     = (t->c_oflag & YOS_OFLAG_ONLCR)  != 0;

    cfg.vmin_      = t->c_cc[YOS_VMIN];
    cfg.vtime_     = t->c_cc[YOS_VTIME];
    cfg.vintr_     = t->c_cc[YOS_VINTR];
    cfg.vquit_     = t->c_cc[YOS_VQUIT];
    cfg.vsusp_     = t->c_cc[YOS_VSUSP];

    cfg.veof_      = 0x04u;

    return cfg;
}

static void tty_update_ldisc_locked(tty_t* tty) {
    ldisc_config_t cfg = config_from_termios(&tty->termios);

    ldisc_set_config(tty->ldisc, &cfg);
}

tty_t* tty_alloc(const tty_driver_ops_t* ops, void* driver_data) {
    tty_t* tty = (tty_t*)kmalloc(sizeof(tty_t));

    if (unlikely(!tty)) {
        return 0;
    }

    memset(tty, 0, sizeof(*tty));

    tty->refs        = 1u;
    tty->ops         = ops;
    tty->driver_data = driver_data;

    spinlock_init(&tty->lock);

    poll_waitq_init_finalizable(
        &tty->poll_waitq,
        tty_poll_waitq_finalize,
        tty
    );

    tty->winsz.ws_row = 25u;
    tty->winsz.ws_col = 80u;

    tty->termios.c_iflag = YOS_IFLAG_ICRNL;
    tty->termios.c_oflag = YOS_OFLAG_OPOST | YOS_OFLAG_ONLCR;
    tty->termios.c_lflag = YOS_LFLAG_ECHO | YOS_LFLAG_ISIG | YOS_LFLAG_ICANON;

    tty->termios.c_cc[YOS_VINTR] = 0x03u;
    tty->termios.c_cc[YOS_VQUIT] = 0x1Cu;
    tty->termios.c_cc[YOS_VSUSP] = 0x1Au;
    tty->termios.c_cc[YOS_VMIN]  = 1u;
    tty->termios.c_cc[YOS_VTIME] = 0u;

    tty->ldisc = ldisc_create();

    if (unlikely(!tty->ldisc)) {
        poll_waitq_detach_all(&tty->poll_waitq);

        poll_waitq_put(&tty->poll_waitq);
        return 0;
    }

    ldisc_set_callbacks(
        tty->ldisc,
        tty_echo_callback, tty,
        tty_signal_callback, tty
    );

    tty_update_ldisc_locked(tty);

    return tty;
}

void tty_retain(tty_t* tty) {
    if (unlikely(!tty)) {
        return;
    }

    for (;;) {
        uint32_t expected = __atomic_load_n(&tty->refs, __ATOMIC_RELAXED);
        
        if (unlikely(expected == 0u)) {
            panic("TTY: tty_retain called after free");
        }

        if (__atomic_compare_exchange_n(
                &tty->refs, &expected, expected + 1u,
                0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
            )) {
            return;
        }
    }
}

static void tty_destroy(tty_t* tty) {
    if (unlikely(!tty)) {
        return;
    }

    if (tty->ops
        && tty->ops->close) {
        tty->ops->close(tty);
    }

    if (tty->ldisc) {
        ldisc_destroy(tty->ldisc);

        tty->ldisc = 0;
    }

    poll_waitq_detach_all(&tty->poll_waitq);

    poll_waitq_put(&tty->poll_waitq);
}

void tty_release(tty_t* tty) {
    if (unlikely(!tty)) {
        return;
    }

    for (;;) {
        uint32_t expected = __atomic_load_n(&tty->refs, __ATOMIC_RELAXED);
        
        if (unlikely(expected == 0u)) {
            panic("TTY: tty_release underflow");
        }

        const uint32_t desired = expected - 1u;

        if (__atomic_compare_exchange_n(
                &tty->refs, &expected, desired,
                0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
            )) {

            if (desired == 0u) {
                tty_destroy(tty);
            }

            return;
        }
    }
}

size_t tty_receive(tty_t* tty, const uint8_t* data, uint32_t size) {
    if (unlikely(!tty
        || !data
        || size == 0u)) {
        return 0;
    }

    size_t w = 0;

    if (likely(tty->ldisc)) {
        w = ldisc_receive(tty->ldisc, data, size);

        if (w > 0) {
            poll_waitq_wake_all(&tty->poll_waitq, VFS_POLLIN);
        }
    }
    
    return w;
}

static void tty_private_retain(void* private_data) {
    tty_retain((tty_t*)private_data);
}

static void tty_private_release(void* private_data) {
    tty_release((tty_t*)private_data);
}

void tty_bind_vfs_node(tty_t* tty, vfs_node_t* node) {
    if (unlikely(!tty
        || !node)) {
        return;
    }

    tty_retain(tty);
    
    node->ops = &g_tty_core_vfs_ops;
    node->private_data = tty;
    node->private_retain = tty_private_retain;
    node->private_release = tty_private_release;

    tty->vfs_node = node;
}

static int tty_check_job_control(tty_t* tty, int is_write) {
    task_t* curr = proc_current();

    if (!curr
        || !curr->controlling_tty) {
        return 0;
    }

    if (curr->controlling_tty->private_data != tty) {
        return 0;
    }

    uint32_t fg = 0u;

    yos_termios_t termios;

    {
        guard_spinlock_safe(&tty->lock);

        fg = tty->fg_pgid;
        
        termios = tty->termios;
    }

    if (fg != 0u && curr->pgid != fg) {
        if (is_write) {
            if ((termios.c_lflag & YOS_LFLAG_TOSTOP) != 0) {
                (void)proc_signal_pgrp(curr->pgid, SIGTTOU);

                sched_yield();
                
                return -1;
            }
        } else {
            (void)proc_signal_pgrp(curr->pgid, SIGTTIN);

            sched_yield();
            
            return -1;
        }
    }

    return 0;
}

static int tty_vfs_read(vfs_node_t* node, ___unused uint32_t offset, uint32_t size, void* buffer) {
    if (unlikely(!node
        || !buffer
        || size == 0u)) {
        return 0;
    }

    tty_t* tty = (tty_t*)node->private_data;

    if (tty_check_job_control(tty, 0) != 0) {
        return -1;
    }

    size_t n = ldisc_read(tty->ldisc, buffer, size);
    
    if (n == (size_t)-2) {
        return -2;
    }

    return (int)n;
}

static int tty_vfs_write(vfs_node_t* node, ___unused uint32_t offset, uint32_t size, const void* buffer) {
    if (unlikely(!node
        || !buffer
        || size == 0u)) {
        return 0;
    }

    tty_t* tty = (tty_t*)node->private_data;

    if (tty_check_job_control(tty, 1) != 0) {
        return -1;
    }

    size_t n = ldisc_write_transform(
        tty->ldisc, buffer, size, 
        tty_echo_callback, tty
    );

    return (int)n;
}

static int tty_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    if (unlikely(!node)) {
        return -1;
    }

    tty_t* tty = (tty_t*)node->private_data;

    task_t* curr = proc_current();

    switch (req) {
        case YOS_TCGETS: {
            if (!arg) {
                return -1;
            }
            
            guard_spinlock_safe(&tty->lock);

            memcpy(arg, &tty->termios, sizeof(tty->termios));
            return 0;
        }

        case YOS_TCSETS: {
            if (!arg) {
                return -1;
            }
            
            yos_termios_t old_termios;

            {
                guard_spinlock_safe(&tty->lock);

                old_termios = tty->termios;
                
                memcpy(&tty->termios, arg, sizeof(tty->termios));

                tty_update_ldisc_locked(tty);
            }

            if (tty->ops && tty->ops->set_termios) {
                tty->ops->set_termios(tty, &old_termios);
            }
            
            return 0;
        }

        case YOS_TIOCGWINSZ: {
            if (!arg) {
                return -1;
            }
            
            guard_spinlock_safe(&tty->lock);
            
            memcpy(arg, &tty->winsz, sizeof(tty->winsz));
            return 0;
        }

        case YOS_TIOCGSID: {
            if (!arg) {
                return -1;
            }
            
            guard_spinlock_safe(&tty->lock);

            *(uint32_t*)arg = tty->session_sid;
            return 0;
        }

        case YOS_TIOCSWINSZ: {
            if (!arg) {
                return -1;
            }

            {
                guard_spinlock_safe(&tty->lock);

                memcpy(&tty->winsz, arg, sizeof(tty->winsz));
            }

            return 0;
        }

        case YOS_TIOCSCTTY: {
            if (!curr
                || curr->pid != curr->sid
                || curr->controlling_tty) {
                return -1;
            }

            vfs_node_retain(node);
            curr->controlling_tty = node;

            guard_spinlock_safe(&tty->lock);

            tty->session_sid = curr->sid;
            
            if (tty->fg_pgid == 0u) {
                tty->fg_pgid = curr->pgid;
            }

            return 0;
        }

        case YOS_TCGETPGRP: {
            if (!arg) {
                return -1;
            }
            
            guard_spinlock_safe(&tty->lock);

            *(uint32_t*)arg = tty->fg_pgid;
            return 0;
        }

        case YOS_TCSETPGRP: {
            if (!arg
                || !curr) {
                return -1;
            }

            uint32_t pgid = *(uint32_t*)arg;
            
            if (pgid == 0u) {
                return -1;
            }

            guard_spinlock_safe(&tty->lock);
            
            if (tty->session_sid != 0u && tty->session_sid != curr->sid) {
                return -1;
            }

            if (!proc_pgrp_in_session(pgid, curr->sid)) {
                return -1;
            }

            tty->fg_pgid = pgid;
            return 0;
        }

        default: {
            if (tty->ops && tty->ops->ioctl) {
                return tty->ops->ioctl(tty, req, arg);
            }

            return -1;
        }
    }
}

static int tty_vfs_poll_status(vfs_node_t* node, int events) {
    if (unlikely(!node)) {
        return 0;
    }
    
    tty_t* tty = (tty_t*)node->private_data;
    
    if (unlikely(!tty)) {
        return VFS_POLLHUP;
    }

    int rev = 0;

    if ((events & VFS_POLLIN) != 0) {
        if (ldisc_has_readable(tty->ldisc)) {
            rev |= VFS_POLLIN;
        }
    }

    if (tty->ops && tty->ops->poll_status) {
        rev |= tty->ops->poll_status(tty, events);
    } else if ((events & VFS_POLLOUT) != 0) {
        rev |= VFS_POLLOUT; 
    }

    return rev;
}

static int tty_vfs_poll_register(vfs_node_t* node, poll_waiter_t* w, task_t* task) {
    if (unlikely(!node
        || !w
        || !task)) {
        return -1;
    }

    tty_t* tty = (tty_t*)node->private_data;

    return poll_waitq_register(&tty->poll_waitq, w, task);
}
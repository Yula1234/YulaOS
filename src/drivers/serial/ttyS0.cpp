#include <drivers/serial/ttyS0.h>

#include <drivers/serial/serial_core.h>

#include <fs/vfs.h>
#include <lib/string.h>
#include <yos/ioctl.h>

#include <kernel/term/term.h>
#include <kernel/tty/tty_internal.h>
#include <kernel/tty/tty_service.h>
#include <kernel/tty/line_discipline.h>

#include <kernel/proc.h>
#include <kernel/poll_waitq.h>
#include <kernel/sched.h>

#include <stddef.h>
#include <stdint.h>

namespace {

static kernel::tty::LineDiscipline g_ld;
static yos_termios_t g_termios;

static poll_waitq_t g_poll_waitq;

struct TtyProcState {
    kernel::SpinLock lock;
    uint32_t session_sid = 0;
    uint32_t fg_pgid = 0;
};

static TtyProcState g_proc;

static void tty_private_retain(void*) {
}

static void tty_private_release(void*) {
}

static bool is_same_tty(const vfs_node_t* a, const vfs_node_t* b) {
    if (!a || !b) {
        return false;
    }

    return a->ops == b->ops && a->private_data == b->private_data;
}

static kernel::term::Term* active_term(void) {
    tty_handle_t* active = kernel::tty::TtyService::instance().get_active_for_render();
    return tty_term_ptr(active);
}

static size_t serial_emit(const uint8_t* data, size_t size, void* ctx) {
    (void)ctx;
    return serial_core_write(data, size);
}

static size_t echo_emit(const uint8_t* data, size_t size, void* ctx) {
    (void)ctx;

    size_t w = serial_core_write(data, size);

    kernel::term::Term* term = active_term();
    if (term && size != 0) {
        term->write((const char*)data, (uint32_t)size);
        kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
    }

    return w;
}

static void drain_rx(void) {
    uint8_t buf[64];

    for (;;) {
        serial_core_poll();

        size_t n = serial_core_read(buf, sizeof(buf));
        if (n == 0) {
            break;
        }

        g_ld.receive_bytes(buf, n);
        poll_waitq_wake_all(&g_poll_waitq);
    }
}

static kernel::tty::LineDisciplineConfig config_from_termios(const yos_termios_t& t) {
    kernel::tty::LineDisciplineConfig cfg;

    cfg.canonical = (t.c_lflag & YOS_LFLAG_ICANON) != 0;
    cfg.echo = (t.c_lflag & YOS_LFLAG_ECHO) != 0;
    cfg.isig = (t.c_lflag & YOS_LFLAG_ISIG) != 0;

    cfg.igncr = (t.c_iflag & YOS_IFLAG_IGNCR) != 0;
    cfg.icrnl = (t.c_iflag & YOS_IFLAG_ICRNL) != 0;
    cfg.inlcr = (t.c_iflag & YOS_IFLAG_INLCR) != 0;

    cfg.opost = (t.c_oflag & YOS_OFLAG_OPOST) != 0;
    cfg.onlcr = (t.c_oflag & YOS_OFLAG_ONLCR) != 0;

    cfg.vmin = t.c_cc[YOS_VMIN];
    cfg.vtime = t.c_cc[YOS_VTIME];

    cfg.vintr = t.c_cc[YOS_VINTR];
    cfg.vquit = t.c_cc[YOS_VQUIT];
    cfg.vsusp = t.c_cc[YOS_VSUSP];

    return cfg;
}

static void tty_signal_emit(int sig, void*) {
    task_t* curr = proc_current();
    if (!curr) {
        return;
    }

    uint32_t pgid = 0;
    {
        kernel::SpinLockSafeGuard g(g_proc.lock);
        pgid = g_proc.fg_pgid;
    }

    if (pgid == 0) {
        pgid = curr->pgid;
    }

    if (pgid != 0) {
        (void)proc_signal_pgrp(pgid, (uint32_t)sig);
    }
}

int ttyS0_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)offset;

    if (!buffer || size == 0) {
        return 0;
    }

    task_t* curr = proc_current();
    if (curr && curr->controlling_tty && is_same_tty(curr->controlling_tty, node)) {
        uint32_t fg = 0;
        {
            kernel::SpinLockSafeGuard g(g_proc.lock);
            fg = g_proc.fg_pgid;
        }

        if (fg != 0 && curr->pgid != fg) {
            (void)proc_signal_pgrp(curr->pgid, SIGTTIN);
            sched_yield();
            return -1;
        }
    }

    for (;;) {
        drain_rx();

        if (g_ld.has_readable()) {
            break;
        }

        proc_usleep(2000);
    }

    size_t n = g_ld.read(buffer, size);
    return (int)n;
}

int ttyS0_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)offset;

    if (!buffer || size == 0) {
        return 0;
    }

    task_t* curr = proc_current();
    if (curr && curr->controlling_tty && is_same_tty(curr->controlling_tty, node)) {
        uint32_t fg = 0;
        {
            kernel::SpinLockSafeGuard g(g_proc.lock);
            fg = g_proc.fg_pgid;
        }

        const int is_bg = (fg != 0 && curr->pgid != fg);
        const int tostop = (g_termios.c_lflag & YOS_LFLAG_TOSTOP) != 0;

        if (is_bg && tostop) {
            (void)proc_signal_pgrp(curr->pgid, SIGTTOU);
            sched_yield();
            return -1;
        }
    }

    serial_core_poll();

    size_t n = g_ld.write_transform(buffer, size, serial_emit, 0);

    kernel::term::Term* term = active_term();
    if (term && size != 0) {
        term->write((const char*)buffer, size);
        kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
    }

    return (int)n;
}

int ttyS0_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;

    task_t* curr = proc_current();

    if (req == YOS_TCGETS) {
        if (!arg) {
            return -1;
        }

        memcpy(arg, &g_termios, sizeof(g_termios));

        return 0;
    }

    if (req == YOS_TCSETS) {
        if (!arg) {
            return -1;
        }

        memcpy(&g_termios, arg, sizeof(g_termios));

        kernel::tty::LineDisciplineConfig cfg = config_from_termios(g_termios);
        g_ld.set_config(cfg);

        return 0;
    }

    if (req == YOS_TIOCSCTTY) {
        if (!curr) {
            return -1;
        }

        if (curr->pid != curr->sid) {
            return -1;
        }

        if (curr->controlling_tty) {
            return -1;
        }

        vfs_node_retain(node);
        curr->controlling_tty = node;

        {
            kernel::SpinLockSafeGuard g(g_proc.lock);
            g_proc.session_sid = curr->sid;
            if (g_proc.fg_pgid == 0) {
                g_proc.fg_pgid = curr->pgid;
            }
        }

        return 0;
    }

    if (req == YOS_TCGETPGRP) {
        if (!arg) {
            return -1;
        }

        uint32_t pgid = 0;
        {
            kernel::SpinLockSafeGuard g(g_proc.lock);
            pgid = g_proc.fg_pgid;
        }

        *(uint32_t*)arg = pgid;
        return 0;
    }

    if (req == YOS_TCSETPGRP) {
        if (!arg || !curr) {
            return -1;
        }

        uint32_t pgid = *(uint32_t*)arg;
        if (pgid == 0) {
            return -1;
        }

        {
            kernel::SpinLockSafeGuard g(g_proc.lock);
            if (g_proc.session_sid != 0 && g_proc.session_sid != curr->sid) {
                return -1;
            }

            if (!proc_pgrp_in_session(pgid, curr->sid)) {
                return -1;
            }

            g_proc.fg_pgid = pgid;
        }

        return 0;
    }

    return -1;
}

vfs_ops_t ttyS0_ops = {
    ttyS0_vfs_read,
    ttyS0_vfs_write,
    0,
    0,
    ttyS0_vfs_ioctl,
};

vfs_node_t ttyS0_node = {
    "ttyS0",
    0,
    0,
    0,
    0,
    &ttyS0_ops,
    &g_proc,
    tty_private_retain,
    tty_private_release,
};

}

extern "C" void ttyS0_init(void) {
    memset(&g_termios, 0, sizeof(g_termios));

    g_termios.c_iflag = YOS_IFLAG_ICRNL;
    g_termios.c_oflag = YOS_OFLAG_OPOST | YOS_OFLAG_ONLCR;
    g_termios.c_lflag = YOS_LFLAG_ECHO | YOS_LFLAG_ISIG | YOS_LFLAG_ICANON;

    g_termios.c_cc[YOS_VINTR] = 0x03u;
    g_termios.c_cc[YOS_VQUIT] = 0x1Cu;
    g_termios.c_cc[YOS_VSUSP] = 0x1Au;

    g_termios.c_cc[YOS_VMIN] = 1u;
    g_termios.c_cc[YOS_VTIME] = 0u;

    g_ld.set_echo_emitter(echo_emit, 0);
    g_ld.set_signal_emitter(tty_signal_emit, 0);
    g_ld.set_config(config_from_termios(g_termios));

    poll_waitq_init(&g_poll_waitq);

    devfs_register(&ttyS0_node);
}

extern "C" int ttyS0_poll_ready(void) {
    drain_rx();
    return g_ld.has_readable() ? 1 : 0;
}

extern "C" int ttyS0_poll_waitq_register(poll_waiter_t* w, task_t* task) {
    if (!w || !task) {
        return -1;
    }

    return poll_waitq_register(&g_poll_waitq, w, task);
}

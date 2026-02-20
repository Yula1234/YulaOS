#include <stdint.h>

#include <hal/irq.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <hal/lock.h>
#include <yos/ioctl.h>

#include <drivers/console.h>
#include <drivers/vga.h>

#include <kernel/tty/tty_internal.h>
#include <kernel/tty/tty_service.h>

#include "console.h"
#include "vga.h"

namespace {

int console_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;

    task_t* curr = proc_current();
    if (!curr || !curr->terminal) {
        return -1;
    }

    tty_handle_t* tty = (tty_handle_t*)curr->terminal;

    if (!buffer || size == 0) {
        return 0;
    }

    const char* char_buf = (const char*)buffer;

    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return -1;
    }

    term->write(char_buf, size);

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);

    return (int)size;
}

int console_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;

    task_t* curr = proc_current();
    if (!curr || !curr->terminal) {
        return -1;
    }

    tty_handle_t* tty = (tty_handle_t*)curr->terminal;

    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return -1;
    }

    if (req == YOS_TCGETS) {
        if (!arg) {
            return -1;
        }

        yos_termios_t* t = (yos_termios_t*)arg;
        memset(t, 0, sizeof(*t));

        return 0;
    }

    if (req == YOS_TCSETS) {
        return 0;
    }

    if (req == YOS_TIOCGWINSZ) {
        if (!arg) {
            return -1;
        }

        uint16_t cols = 0;
        uint16_t rows = 0;
        if (term->get_winsz(cols, rows) != 0) {
            return -1;
        }

        yos_winsize_t* ws = (yos_winsize_t*)arg;
        ws->ws_col = cols;
        ws->ws_row = rows;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;

        return 0;
    }

    if (req == YOS_TIOCSWINSZ) {
        if (!arg) {
            return -1;
        }

        const yos_winsize_t* ws = (const yos_winsize_t*)arg;
        if (term->set_winsz(ws->ws_col, ws->ws_row) != 0) {
            return -1;
        }

        kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Resize);

        return 0;
    }

    if (req == YOS_TTY_SCROLL) {
        if (!arg) {
            return -1;
        }

        yos_tty_scroll_t* s = (yos_tty_scroll_t*)arg;
        int rc = term->scroll(s->delta);
        if (rc == 0) {
            kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Scroll);
        }

        return rc;
    }

    return -1;
}

vfs_ops_t console_ops = {
    0,
    console_vfs_write,
    0,
    0,
    console_vfs_ioctl,
};

vfs_node_t console_node = {
    "console",
    0,
    0,
    0,
    0,
    &console_ops,
    0,
    0,
    0,
};

}

extern "C" void console_init() {
    devfs_register(&console_node);
}

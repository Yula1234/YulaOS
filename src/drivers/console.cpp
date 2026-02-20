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

#include <kernel/tty/tty_api.h>

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

    tty_write(tty, char_buf, size);

    return (int)size;
}

int console_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;

    task_t* curr = proc_current();
    if (!curr || !curr->terminal) {
        return -1;
    }

    tty_handle_t* tty = (tty_handle_t*)curr->terminal;

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

        return tty_get_winsz(tty, (yos_winsize_t*)arg);
    }

    if (req == YOS_TIOCSWINSZ) {
        if (!arg) {
            return -1;
        }

        return tty_set_winsz(tty, (const yos_winsize_t*)arg);
    }

    if (req == YOS_TTY_SCROLL) {
        if (!arg) {
            return -1;
        }

        yos_tty_scroll_t* s = (yos_tty_scroll_t*)arg;
        return tty_scroll(tty, s->delta);
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

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

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

#include "console.h"
#include "vga.h"


static int console_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;  
    (void)offset;
    
    task_t* curr = proc_current();
    if (!curr->terminal) return -1;

    term_instance_t* term = (term_instance_t*)curr->terminal;

    const char* char_buf = (const char*)buffer;

    const uint32_t BATCH = 256;
    uint32_t pos = 0;

    while (pos < size) {
        uint32_t end = pos + BATCH;
        if (end > size) end = size;

        spinlock_acquire(&term->lock);

        term_write(term, &char_buf[pos], end - pos);

        spinlock_release(&term->lock);
        pos = end;
    }

    return size;
}

static int console_vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;

    task_t* curr = proc_current();
    if (!curr || !curr->terminal) return -1;

    term_instance_t* term = (term_instance_t*)curr->terminal;

    if (req == YOS_TCGETS) {
        if (!arg) return -1;
        yos_termios_t* t = (yos_termios_t*)arg;
        memset(t, 0, sizeof(*t));
        return 0;
    }

    if (req == YOS_TCSETS) {
        return 0;
    }

    if (req == YOS_TIOCGWINSZ) {
        if (!arg) return -1;
        yos_winsize_t* ws = (yos_winsize_t*)arg;
        spinlock_acquire(&term->lock);
        ws->ws_col = (uint16_t)(term->cols > 0 ? term->cols : 1);
        ws->ws_row = (uint16_t)(term->view_rows > 0 ? term->view_rows : 1);
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        spinlock_release(&term->lock);
        return 0;
    }

    if (req == YOS_TIOCSWINSZ) {
        if (!arg) return -1;
        yos_winsize_t* ws = (yos_winsize_t*)arg;
        spinlock_acquire(&term->lock);
        if (ws->ws_col > 0) term->cols = (int)ws->ws_col;
        if (ws->ws_row > 0) term->view_rows = (int)ws->ws_row;
        term_reflow(term, term->cols);
        spinlock_release(&term->lock);
        return 0;
    }

    if (req == YOS_TTY_SCROLL) {
        if (!arg) return -1;
        yos_tty_scroll_t* s = (yos_tty_scroll_t*)arg;
        spinlock_acquire(&term->lock);
        int view_rows = term->view_rows;
        if (view_rows < 1) view_rows = 1;
        int max_view_row = term->max_row - view_rows + 1;
        if (max_view_row < 0) max_view_row = 0;

        int old_view_row = term->view_row;

        if (s->delta == 0) {
            term->view_row = max_view_row;
        } else if (s->delta > 0) {
            int next = term->view_row - s->delta;
            if (next < 0) next = 0;
            term->view_row = next;
        } else {
            int next = term->view_row - s->delta;
            if (next > max_view_row) next = max_view_row;
            term->view_row = next;
        }

        if (term->view_row != old_view_row) {
            term_invalidate_view(term);
        }

        spinlock_release(&term->lock);
        return 0;
    }

    return -1;
}

static vfs_ops_t console_ops = { .write = console_vfs_write, .ioctl = console_vfs_ioctl };
static vfs_node_t console_node = { .name = "console", .ops = &console_ops };

void console_init() {
    devfs_register(&console_node);
}

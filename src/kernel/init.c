// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/string.h>

#include <fs/yulafs.h>
#include <fs/vfs.h>
#include <fs/bcache.h>

#include <drivers/uhci.h>
#include <drivers/vga.h>

#include <kernel/tty/tty.h>
#include <kernel/tty/tty_bridge.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/input_focus.h>
#include <kernel/cpu.h>
#include <kernel/output/kprintf.h>

#include <mm/heap.h>

#include <hal/lock.h>
#include <hal/io.h>

#include "init.h"

static void init_task_prepare_dirs(void) {
    if (yulafs_lookup("/bin") == -1) (void)yulafs_mkdir("/bin");
    if (yulafs_lookup("/home") == -1) (void)yulafs_mkdir("/home");
}

static tty_handle_t* init_task_create_terminal(task_t* self) {
    tty_handle_t* tty = tty_bridge_create_default();
    if (!tty) return 0;

    self->terminal = tty;
    self->term_mode = 1;

    tty_bridge_set_active(tty);
    return tty;
}

static void init_task_open_devices(void) {
    (void)vfs_open("/dev/kbd", 0);
    (void)vfs_open("/dev/console", 0);
    (void)vfs_open("/dev/console", 0);
}

static void init_task_set_cwd(task_t* self) {
    uint32_t home_inode = (uint32_t)yulafs_lookup("/home");
    if ((int)home_inode == -1) home_inode = 1;
    self->cwd_inode = home_inode;
}

static void init_append_int(char* out, int* io_pos, int cap, int v) {
    if (!out || !io_pos || cap <= 0) {
        return;
    }

    int pos = *io_pos;
    if (pos < 0 || pos >= cap) {
        return;
    }

    uint32_t u = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
    if (v < 0) {
        if (pos + 1 < cap) {
            out[pos++] = '-';
        }
    }

    char tmp[11];
    int n = 0;
    if (u == 0) {
        tmp[n++] = '0';
    } else {
        while (u != 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (u % 10u));
            u /= 10u;
        }
    }

    while (n > 0 && pos + 1 < cap) {
        out[pos++] = tmp[--n];
    }

    *io_pos = pos;
}

static void init_task_spawn_shell_loop(task_t* self, tty_handle_t* tty) {
    for (;;) {
        char* argv[] = { "ush", 0 };
        task_t* child = proc_spawn_elf("/bin/ush.exe", 1, argv);
        if (!child) {
            tty_bridge_print(tty, "init: failed to spawn /bin/ush.exe\n");
            proc_usleep(200000);
            continue;
        }

        input_focus_set_pid(child->pid);
        int st = 0;
        (void)proc_waitpid(child->pid, &st);
        input_focus_set_pid(self->pid);

        if (st != 0) {
            char msg[48];
            int pos = 0;
            const char prefix[] = "[ush exited: ";
            const char suffix[] = "]\n";

            for (uint32_t i = 0; i < (uint32_t)(sizeof(prefix) - 1u) && pos + 1 < (int)sizeof(msg); i++) {
                msg[pos++] = prefix[i];
            }
            init_append_int(msg, &pos, (int)sizeof(msg), st);
            for (uint32_t i = 0; i < (uint32_t)(sizeof(suffix) - 1u) && pos + 1 < (int)sizeof(msg); i++) {
                msg[pos++] = suffix[i];
            }
            msg[pos] = '\0';

            tty_bridge_print(tty, msg);
        }

        proc_usleep(200000);
    }
}

void init_task(void* arg) {
    (void)arg;

    kprintf("Booted\n");

    init_task_prepare_dirs();
    task_t* self = proc_current();
    if (!self) return;

    tty_handle_t* tty = init_task_create_terminal(self);
    if (!tty) return;

    init_task_open_devices();
    tty_bridge_putc(tty, 0x0C);

    init_task_set_cwd(self);
    init_task_spawn_shell_loop(self, tty);
}

void uhci_late_init_task(void* arg) {
    (void)arg;
    uhci_init();
    uhci_late_init();

    while (1) {
        uhci_poll();
        proc_usleep(2000);
    }
}

void idle_task_func(void* arg) {
    (void)arg;
    while (1) {
        __asm__ volatile("sti");
        cpu_hlt();
        sched_yield();
    }
}

void syncer_task(void* arg) {
    (void)arg;
    while (1) {
        proc_usleep(400000);
        bcache_sync();
    }
}

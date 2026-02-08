// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/string.h>

#include <fs/yulafs.h>
#include <fs/vfs.h>
#include <fs/bcache.h>

#include <drivers/uhci.h>
#include <drivers/vga.h>

#include <kernel/tty.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/input_focus.h>
#include <kernel/cpu.h>

#include <mm/heap.h>

#include <hal/lock.h>
#include <hal/io.h>

#include "init.h"

static void init_task_prepare_dirs(void) {
    if (yulafs_lookup("/bin") == -1) (void)yulafs_mkdir("/bin");
    if (yulafs_lookup("/home") == -1) (void)yulafs_mkdir("/home");
}

static term_instance_t* init_task_create_terminal(task_t* self) {
    term_instance_t* term = (term_instance_t*)kmalloc(sizeof(*term));
    if (!term) return 0;
    memset(term, 0, sizeof(*term));

    tty_term_apply_default_size(term);
    term->curr_fg = 0xD4D4D4;
    term->curr_bg = 0x141414;
    term_init(term);

    self->terminal = term;
    self->term_mode = 1;
    tty_set_terminal(term);

    return term;
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

static void init_task_spawn_shell_loop(task_t* self, term_instance_t* term) {
    for (;;) {
        char* argv[] = { "ush", 0 };
        task_t* child = proc_spawn_elf("/bin/ush.exe", 1, argv);
        if (!child) {
            tty_term_print_locked(term, "init: failed to spawn /bin/ush.exe\n");
            proc_usleep(200000);
            continue;
        }

        input_focus_set_pid(child->pid);
        proc_wait(child->pid);
        input_focus_set_pid(self->pid);

        tty_term_print_locked(term, "[ush exited]\n");
        proc_usleep(200000);
    }
}

void init_task(void* arg) {
    (void)arg;

    init_task_prepare_dirs();
    task_t* self = proc_current();
    if (!self) return;

    term_instance_t* term = init_task_create_terminal(self);
    if (!term) return;

    init_task_open_devices();
    spinlock_acquire(&term->lock);
    term_putc(term, 0x0C);
    spinlock_release(&term->lock);

    init_task_set_cwd(self);
    init_task_spawn_shell_loop(self, term);
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

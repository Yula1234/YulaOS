/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_TTY_CORE_H
#define KERNEL_TTY_CORE_H

#include <kernel/locking/spinlock.h>
#include <kernel/waitq/poll_waitq.h>
#include <kernel/tty/ldisc.h>

#include <yos/ioctl.h>

#include <fs/vfs.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tty;
typedef struct tty tty_t;

typedef struct {
    int (*open)(tty_t* tty);

    int (*write)(tty_t* tty, const void* buf, uint32_t size);

    int (*ioctl)(tty_t* tty, uint32_t req, void* arg);

    void (*close)(tty_t* tty);

    void (*set_termios)(tty_t* tty, const yos_termios_t* old_termios);

    int (*poll_status)(tty_t* tty, int events);
} tty_driver_ops_t;

struct tty {
    volatile uint32_t refs;

    spinlock_t lock;

    uint32_t session_sid;
    uint32_t fg_pgid;

    yos_termios_t termios;
    yos_winsize_t winsz;

    ldisc_t* ldisc;

    poll_waitq_t poll_waitq;

    const tty_driver_ops_t* ops;

    void* driver_data;

    vfs_node_t* vfs_node;
};

tty_t* tty_alloc(const tty_driver_ops_t* ops, void* driver_data);

void tty_retain(tty_t* tty);
void tty_release(tty_t* tty);

size_t tty_receive(tty_t* tty, const uint8_t* data, uint32_t size);

void tty_bind_vfs_node(tty_t* tty, vfs_node_t* node);

#ifdef __cplusplus
}
#endif

#endif
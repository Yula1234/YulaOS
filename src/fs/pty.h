// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef FS_PTY_H
#define FS_PTY_H

#include <kernel/poll_waitq.h>

#include "vfs.h"

struct task;

void pty_init(void);

int pty_poll_info(vfs_node_t* node, uint32_t* out_available, uint32_t* out_space, int* out_peer_open);
int pty_poll_waitq_register(vfs_node_t* node, poll_waiter_t* w, struct task* task);

#endif

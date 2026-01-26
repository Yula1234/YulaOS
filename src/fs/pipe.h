// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef FS_PIPE_H
#define FS_PIPE_H

#include <kernel/poll_waitq.h>

#include "vfs.h"

int vfs_create_pipe(vfs_node_t** read_node, vfs_node_t** write_node);

int pipe_read_nonblock(vfs_node_t* node, uint32_t size, void* buffer);

int pipe_write_nonblock(vfs_node_t* node, uint32_t size, const void* buffer);

int pipe_poll_info(vfs_node_t* node, uint32_t* out_available, uint32_t* out_space, int* out_readers, int* out_writers);

int pipe_poll_waitq_register(vfs_node_t* node, poll_waiter_t* w, struct task* task);

#endif
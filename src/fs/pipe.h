/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef FS_PIPE_H
#define FS_PIPE_H

#include <kernel/poll_waitq.h>

#include "vfs.h"

/*
 * VFS pipes.
 *
 * This module implements a classic byte-stream pipe on top of a fixed-size
 * in-kernel ring buffer.
 *
 * The pipe is exposed as two VFS nodes:
 *  - a read end (VFS_FLAG_PIPE_READ)
 *  - a write end (VFS_FLAG_PIPE_WRITE)
 *
 * The implementation is synchronous and is built from:
 *  - a spinlock protecting ring indices and reader/writer counters
 *  - two counting semaphores modelling readable bytes and writable space
 *  - a poll waitqueue for edge notifications
 */

#ifdef __cplusplus
extern "C" {
#endif

int vfs_create_pipe(vfs_node_t** read_node, vfs_node_t** write_node);

/*
 * Non-blocking read.
 *
 * Returns:
 *  - >0: number of bytes copied
 *  -  0: would block (no data) while writers still exist
 *  - -1: EOF (no data and no writers) or invalid usage
 */
int pipe_read_nonblock(vfs_node_t* node, uint32_t size, void* buffer);

/*
 * Non-blocking write.
 *
 * Returns:
 *  - >0: number of bytes copied (for this implementation: either 0 or size)
 *  -  0: would block (no space)
 *  - -1: broken pipe (no readers) or invalid usage
 */
int pipe_write_nonblock(vfs_node_t* node, uint32_t size, const void* buffer);

/*
 * Query current pipe state for poll/select.
 *
 * This is a snapshot; callers must treat it as advisory and re-check after
 * registering with the waitqueue.
 */
int pipe_poll_info(vfs_node_t* node, uint32_t* out_available, uint32_t* out_space, int* out_readers, int* out_writers);

/* Register a poll waiter for wakeups on read/write availability changes. */
int pipe_poll_waitq_register(vfs_node_t* node, poll_waiter_t* w, struct task* task);

#ifdef __cplusplus
}
#endif

#endif

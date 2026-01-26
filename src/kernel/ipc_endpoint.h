// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_IPC_ENDPOINT_H
#define KERNEL_IPC_ENDPOINT_H

#include <kernel/poll_waitq.h>
#include <stdint.h>

struct vfs_node;

struct vfs_node* ipc_listen_create(const char* name);

int ipc_accept(struct vfs_node* listen_node, struct vfs_node** out_c2s_r, struct vfs_node** out_s2c_w);

int ipc_listen_poll_ready(struct vfs_node* listen_node);

int ipc_listen_poll_waitq_register(struct vfs_node* listen_node, poll_waiter_t* w, struct task* task);

int ipc_connect(const char* name,
                struct vfs_node** out_c2s_w,
                struct vfs_node** out_s2c_r,
                void** out_pending_handle);

void ipc_connect_cancel(void* pending_handle);

#endif

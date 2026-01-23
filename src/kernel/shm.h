// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_SHM_H
#define KERNEL_SHM_H

#include <stdint.h>

struct vfs_node;

struct vfs_node* shm_create_node(uint32_t size);

struct vfs_node* shm_create_named_node(const char* name, uint32_t size);
struct vfs_node* shm_open_named_node(const char* name);
int shm_unlink_named(const char* name);

#endif

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef FS_PIPE_H
#define FS_PIPE_H

#include "vfs.h"

int vfs_create_pipe(vfs_node_t** read_node, vfs_node_t** write_node);

#endif
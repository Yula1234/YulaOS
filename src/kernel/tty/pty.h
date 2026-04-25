// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef FS_PTY_H
#define FS_PTY_H

#include <kernel/waitq/poll_waitq.h>

#include <fs/vfs.h>

struct task;

#ifdef __cplusplus
extern "C" {
#endif

void pty_init(void);

#ifdef __cplusplus
}
#endif

#endif

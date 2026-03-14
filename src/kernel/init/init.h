// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_INIT_H
#define KERNEL_INIT_H

void init_task(void* arg);
void uhci_late_init_task(void* arg);
void idle_task_func(void* arg);
void syncer_task(void* arg);

#endif

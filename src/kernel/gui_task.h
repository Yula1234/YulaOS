// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_GUI_TASK_H
#define KERNEL_GUI_TASK_H

#include <stdint.h>

void gui_task(void* arg);
void wake_up_gui(void);

#endif
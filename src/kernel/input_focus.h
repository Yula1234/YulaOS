// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_INPUT_FOCUS_H
#define KERNEL_INPUT_FOCUS_H

#include <stdint.h>

uint32_t input_focus_get_pid(void);
void input_focus_set_pid(uint32_t pid);
uint32_t input_focus_exchange_pid(uint32_t pid);

#endif

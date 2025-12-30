// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_CLIPBOARD_H
#define KERNEL_CLIPBOARD_H

void clipboard_init(void);
int clipboard_set(const char* data, int len);
int clipboard_get(char* buf, int max_len);

#endif
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "flux_internal.h"

volatile int g_should_exit;
volatile int g_fb_released;

volatile int g_virgl_active;

uint32_t g_commit_gen = 1;

int g_screen_w = 0;
int g_screen_h = 0;

void dbg_write(const char* s) {
    if (!s) return;
    write(1, s, (uint32_t)strlen(s));
}

int pipe_try_write_frame(int fd, const void* buf, uint32_t size, int essential) {
    if (fd < 0 || !buf || size == 0) return -1;
    (void)essential;

    int wn = pipe_try_write(fd, buf, size);
    if (wn < 0) return -1;
    if (wn == 0) return 0;
    return (wn == (int)size) ? 1 : -1;
}

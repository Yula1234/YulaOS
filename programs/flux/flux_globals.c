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

    const uint8_t* p = (const uint8_t*)buf;
    uint32_t off = 0;
    int tries = 0;

    const int max_tries_initial = essential ? 256 : 1;
    const int max_tries_partial = 4096;

    while (off < size) {
        int wn = pipe_try_write(fd, p + off, size - off);
        if (wn < 0) return -1;
        if (wn == 0) {
            if (off == 0 && !essential) return 0;

            const int max_tries = (off == 0) ? max_tries_initial : max_tries_partial;
            tries++;
            if (tries >= max_tries) {
                return (off == 0) ? 0 : -1;
            }
            usleep(1000);
            continue;
        }

        off += (uint32_t)wn;
        tries = 0;
    }

    return 1;
}

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef DRIVERS_FBDEV_H
#define DRIVERS_FBDEV_H

#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t stride;
    uint32_t bpp;
    uint32_t size_bytes;
} fb_info_t;

extern uint32_t* fb_ptr;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;
extern volatile int g_fb_mapped;

void fb_vfs_init(void);

uint32_t fb_get_owner_pid(void);
int fb_acquire(uint32_t pid);
int fb_release(uint32_t pid);
void fb_release_by_pid(uint32_t pid);
int fb_kernel_can_render(void);

#endif

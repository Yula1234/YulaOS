// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <hal/lock.h>

#include "clipboard.h"

#define CLIPBOARD_SIZE 4096

static char clipboard_data[CLIPBOARD_SIZE];
static int clipboard_len = 0;
static spinlock_t clip_lock;

void clipboard_init() {
    spinlock_init(&clip_lock);
    memset(clipboard_data, 0, CLIPBOARD_SIZE);
    clipboard_len = 0;
}

int clipboard_set(const char* data, int len) {
    if (!data || len < 0) return -1;
    
    if (len > CLIPBOARD_SIZE - 1) len = CLIPBOARD_SIZE - 1;
    
    if (len < 0 || len > CLIPBOARD_SIZE - 1) return -1;
    
    uint32_t flags = spinlock_acquire_safe(&clip_lock);
    
    if (len > 0 && len <= CLIPBOARD_SIZE - 1) {
        memcpy(clipboard_data, data, len);
        clipboard_data[len] = '\0';
        clipboard_len = len;
    } else {
        clipboard_data[0] = '\0';
        clipboard_len = 0;
    }
    
    spinlock_release_safe(&clip_lock, flags);
    return clipboard_len;
}

int clipboard_get(char* buf, int max_len) {
    if (!buf || max_len <= 0) return -1;
    
    uint32_t flags = spinlock_acquire_safe(&clip_lock);
    
    int to_copy = clipboard_len;
    if (to_copy > max_len - 1) to_copy = max_len - 1;
    if (to_copy < 0) to_copy = 0;
    
    if (to_copy > 0 && to_copy < max_len) {
        memcpy(buf, clipboard_data, to_copy);
        buf[to_copy] = '\0';
    } else {
        buf[0] = '\0';
        to_copy = 0;
    }
    
    spinlock_release_safe(&clip_lock, flags);
    return to_copy;
}
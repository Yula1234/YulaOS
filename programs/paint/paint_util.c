// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_util.h"

void dbg_write(const char* s) {
    if (!s) {
        return;
    }
    write(1, s, (uint32_t)strlen(s));
}

int ptr_is_invalid(const void* p) {
    if (!p) {
        return 1;
    }
    if (p == (const void*)-1) {
        return 1;
    }
    return 0;
}

int min_i(int a, int b) {
    if (a < b) {
        return a;
    }
    return b;
}

int max_i(int a, int b) {
    if (a > b) {
        return a;
    }
    return b;
}

int isqrt_i(int v) {
    if (v <= 0) {
        return 0;
    }
    int x = v;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

uint32_t blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) {
        return fg;
    }
    if (alpha == 0) {
        return bg;
    }
    uint32_t r = ((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha);
    uint32_t g = ((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha);
    uint32_t b = (fg & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha);
    return ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
}

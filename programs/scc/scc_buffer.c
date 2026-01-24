// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "scc_buffer.h"

void buf_init(Buffer* b, uint32_t cap) {
    if (cap == 0) cap = 64;
    b->data = (uint8_t*)malloc(cap);
    if (!b->data) {
        printf("Out of memory (buf_init, %u bytes)\n", cap);
        exit(1);
    }
    b->size = 0;
    b->cap = cap;
}

void buf_free(Buffer* b) {
    if (b->data) free(b->data);
    b->data = 0;
    b->size = 0;
    b->cap = 0;
}

void buf_reserve(Buffer* b, uint32_t extra) {
    uint32_t need = b->size + extra;
    if (need <= b->cap) return;

    uint32_t ncap = b->cap;
    while (ncap < need) ncap *= 2;

    uint8_t* nd = (uint8_t*)realloc(b->data, ncap);
    if (!nd) {
        printf("Out of memory (buf_reserve, need %u bytes)\n", ncap);
        exit(1);
    }
    b->data = nd;
    b->cap = ncap;
}

void buf_push_u8(Buffer* b, uint8_t v) {
    buf_reserve(b, 1);
    b->data[b->size++] = v;
}

void buf_push_u16(Buffer* b, uint16_t v) {
    buf_reserve(b, 2);
    b->data[b->size++] = (uint8_t)(v & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 8) & 0xFF);
}

void buf_push_u16_le(Buffer* b, uint16_t v) {
    buf_push_u16(b, v);
}

void buf_push_u32(Buffer* b, uint32_t v) {
    buf_reserve(b, 4);
    b->data[b->size++] = (uint8_t)(v & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 24) & 0xFF);
}

void buf_write(Buffer* b, const void* src, uint32_t len) {
    buf_reserve(b, len);
    memcpy(b->data + b->size, src, len);
    b->size += len;
}

uint32_t buf_add_cstr(Buffer* b, const char* s) {
    uint32_t off = b->size;
    while (*s) buf_push_u8(b, (uint8_t)*s++);
    buf_push_u8(b, 0);
    return off;
}

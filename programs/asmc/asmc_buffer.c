// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_buffer.h"

void buf_init(Buffer* b, uint32_t cap) {
    if (cap == 0) cap = 64;
    b->data = malloc(cap);
    if (!b->data) exit(1);
    b->size = 0;
    b->capacity = cap;
}

void buf_free(Buffer* b) {
    if (b->data) free(b->data);
    b->data = 0; b->size = 0;
}

void buf_push(Buffer* b, uint8_t byte) {
    if (b->size >= b->capacity) {
        b->capacity *= 2;
        uint8_t* new_data = malloc(b->capacity);
        if (!new_data) exit(1);
        memcpy(new_data, b->data, b->size);
        free(b->data);
        b->data = new_data;
    }
    b->data[b->size++] = byte;
}

void buf_push_u32(Buffer* b, uint32_t val) {
    buf_push(b, val & 0xFF);
    buf_push(b, (val >> 8) & 0xFF);
    buf_push(b, (val >> 16) & 0xFF);
    buf_push(b, (val >> 24) & 0xFF);
}

void buf_write(Buffer* b, void* src, uint32_t len) {
    uint8_t* p = (uint8_t*)src;
    for(uint32_t i=0; i<len; i++) buf_push(b, p[i]);
}

uint32_t buf_add_string(Buffer* b, const char* str) {
    uint32_t offset = b->size;
    while (*str) buf_push(b, *str++);
    buf_push(b, 0);
    return offset;
}

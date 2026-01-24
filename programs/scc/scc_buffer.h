// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_BUFFER_H_INCLUDED
#define SCC_BUFFER_H_INCLUDED

#include "scc_common.h"

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t cap;
} Buffer;

void buf_init(Buffer* b, uint32_t cap);
void buf_free(Buffer* b);
void buf_reserve(Buffer* b, uint32_t extra);
void buf_push_u8(Buffer* b, uint8_t v);
void buf_push_u16(Buffer* b, uint16_t v);
void buf_push_u16_le(Buffer* b, uint16_t v);
void buf_push_u32(Buffer* b, uint32_t v);
void buf_write(Buffer* b, const void* src, uint32_t len);
uint32_t buf_add_cstr(Buffer* b, const char* s);

#endif

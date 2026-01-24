// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ASMC_BUFFER_H_INCLUDED
#define ASMC_BUFFER_H_INCLUDED

#include "asmc_core.h"

void buf_init(Buffer* b, uint32_t cap);
void buf_free(Buffer* b);
void buf_push(Buffer* b, uint8_t byte);
void buf_push_u32(Buffer* b, uint32_t val);
void buf_write(Buffer* b, void* src, uint32_t len);
uint32_t buf_add_string(Buffer* b, const char* str);

#endif

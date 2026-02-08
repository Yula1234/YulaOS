// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include "paint_state.h"

void dbg_write(const char* s);
int ptr_is_invalid(const void* p);
int min_i(int a, int b);
int max_i(int a, int b);
int isqrt_i(int v);
uint32_t blend(uint32_t fg, uint32_t bg, uint8_t alpha);

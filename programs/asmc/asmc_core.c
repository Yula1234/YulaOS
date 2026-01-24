// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_core.h"

void panic(AssemblerCtx* ctx, const char* msg) {
    set_console_color(0xF44747, 0x141414);
    printf("\n[ASMC ERROR] Line %d: %s\n", ctx->line_num, msg);
    set_console_color(0xD4D4D4, 0x141414);
    exit(1);
}

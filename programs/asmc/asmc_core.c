// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_core.h"

void panic(AssemblerCtx* ctx, const char* msg) {
    puts("\x1b[91m");
    printf("\n[ASMC ERROR] Line %d: %s\n", ctx->line_num, msg);
    puts("\x1b[0m");
    exit(1);
}

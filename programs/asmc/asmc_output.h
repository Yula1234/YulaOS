// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ASMC_OUTPUT_H_INCLUDED
#define ASMC_OUTPUT_H_INCLUDED

#include "asmc_core.h"

void write_elf(AssemblerCtx* ctx, const char* filename);
void write_binary(AssemblerCtx* ctx, const char* filename);

#endif

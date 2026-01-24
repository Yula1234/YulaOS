// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ASMC_SYMBOLS_H_INCLUDED
#define ASMC_SYMBOLS_H_INCLUDED

#include "asmc_core.h"

void sym_table_init(AssemblerCtx* ctx);
void sym_table_free(AssemblerCtx* ctx);

void normalize_symbol_name(AssemblerCtx* ctx, const char* in, char* out, size_t out_size);
void resolve_symbol_name(AssemblerCtx* ctx, const char* in, char* out, size_t out_size);
uint32_t resolve_abs_addr(AssemblerCtx* ctx, Symbol* s);

Symbol* sym_find(AssemblerCtx* ctx, const char* name);
Symbol* sym_add(AssemblerCtx* ctx, const char* name);
void sym_define_label(AssemblerCtx* ctx, const char* name);

#endif

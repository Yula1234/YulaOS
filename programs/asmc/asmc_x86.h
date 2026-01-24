// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ASMC_X86_H_INCLUDED
#define ASMC_X86_H_INCLUDED

#include "asmc_core.h"

typedef enum { OP_NONE, OP_REG, OP_MEM, OP_IMM } OpType;

typedef struct {
    OpType type;
    int reg;
    int size;
    int32_t disp;
    char label[64];
    int has_label;
    int base_reg;
    int index_reg;
    int scale;
} Operand;

int get_reg_info(const char* s, int* size);

void isa_build_index(void);
void isa_free_index(void);

void parse_operand(AssemblerCtx* ctx, char* text, Operand* op);
void assemble_instr(AssemblerCtx* ctx, char* name, int explicit_size, Operand* o1, Operand* o2);

Buffer* get_cur_buffer(AssemblerCtx* ctx);
void emit_reloc(AssemblerCtx* ctx, int type, char* label, uint32_t offset);

#endif

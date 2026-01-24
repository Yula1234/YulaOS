// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_x86.h"

#include "asmc_buffer.h"
#include "asmc_expr.h"
#include "asmc_symbols.h"

const char* reg_names32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
const char* reg_names16[] = { "ax",  "cx",  "dx",  "bx",  "sp",  "bp",  "si",  "di"  };
const char* reg_names8[]  = { "al",  "cl",  "dl",  "bl",  "ah",  "ch",  "dh",  "bh"  };

int get_reg_info(const char* s, int* size) {
    for(int i=0; i<8; i++) {
        if(strcmp(s, reg_names32[i])==0) { *size = 4; return i; }
        if(strcmp(s, reg_names16[i])==0) { *size = 2; return i; }
        if(strcmp(s, reg_names8[i])==0)  { *size = 1; return i; }
    }
    return -1;
}

static int is_16bit_addr_reg(int reg_index) {
    return reg_index == 3 || reg_index == 5 || reg_index == 6 || reg_index == 7;
}

typedef enum {
    ENC_NONE, ENC_R, ENC_I, ENC_M, ENC_MR, ENC_RM, ENC_MI, ENC_OI, ENC_J, ENC_SHIFT,
    ENC_0F, ENC_0F_MR, ENC_0F_RM
} EncMode;

typedef struct {
    const char* mnem;
    uint8_t op_base;
    uint8_t op_ext;
    EncMode mode;
    int size;
} InstrDef;

InstrDef isa[] = {
    { "ret",   0xC3, 0, ENC_NONE, 0 }, { "nop",   0x90, 0, ENC_NONE, 0 },
    { "hlt",   0xF4, 0, ENC_NONE, 0 }, { "cli",   0xFA, 0, ENC_NONE, 0 },
    { "sti",   0xFB, 0, ENC_NONE, 0 }, { "pusha", 0x60, 0, ENC_NONE, 0 },
    { "popa",  0x61, 0, ENC_NONE, 0 }, { "leave", 0xC9, 0, ENC_NONE, 0 },
    { "cld",   0xFC, 0, ENC_NONE, 0 }, { "std",   0xFD, 0, ENC_NONE, 0 },
    { "int3",  0xCC, 0, ENC_NONE, 0 },

    { "movsb", 0xA4, 0, ENC_NONE, 0 }, { "movsd", 0xA5, 0, ENC_NONE, 0 },
    { "stosb", 0xAA, 0, ENC_NONE, 0 }, { "stosd", 0xAB, 0, ENC_NONE, 0 },
    { "lodsb", 0xAC, 0, ENC_NONE, 0 }, { "lodsd", 0xAD, 0, ENC_NONE, 0 },
    { "cmpsb", 0xA6, 0, ENC_NONE, 0 }, { "cmpsd", 0xA7, 0, ENC_NONE, 0 },
    { "scasb", 0xAE, 0, ENC_NONE, 0 }, { "scasd", 0xAF, 0, ENC_NONE, 0 },

    { "ud2",   0x0B, 0, ENC_0F, 0 },   { "rdtsc", 0x31, 0, ENC_0F, 0 },

    { "push",  0x50, 0, ENC_R, 4 },    { "pop",   0x58, 0, ENC_R, 4 },
    { "push",  0x68, 0, ENC_I, 4 },    { "int",   0xCD, 0, ENC_I, 0 },
    { "push",  0x6A, 0, ENC_I, 1 },

    { "inc",   0x40, 0, ENC_R, 4 },    { "dec",   0x48, 0, ENC_R, 4 },
    { "inc",   0xFE, 0, ENC_M, 1 },    { "dec",   0xFE, 1, ENC_M, 1 },
    { "inc",   0xFF, 0, ENC_M, 4 },    { "dec",   0xFF, 1, ENC_M, 4 },

    { "mul",   0xF6, 4, ENC_M, 1 },    { "imul",  0xF6, 5, ENC_M, 1 },
    { "div",   0xF6, 6, ENC_M, 1 },    { "idiv",  0xF6, 7, ENC_M, 1 },
    { "neg",   0xF6, 3, ENC_M, 1 },    { "not",   0xF6, 2, ENC_M, 1 },

    { "mul",   0xF7, 4, ENC_M, 4 },    { "imul",  0xF7, 5, ENC_M, 4 },
    { "div",   0xF7, 6, ENC_M, 4 },    { "idiv",  0xF7, 7, ENC_M, 4 },
    { "neg",   0xF7, 3, ENC_M, 4 },    { "not",   0xF7, 2, ENC_M, 4 },

    { "call",  0xE8, 0, ENC_J, 0 },    { "jmp",   0xE9, 0, ENC_J, 0 },
    { "call",  0xFF, 2, ENC_M, 4 },
    { "je",    0x84, 0, ENC_J, 0 },    { "jz",    0x84, 0, ENC_J, 0 },
    { "jne",   0x85, 0, ENC_J, 0 },    { "jnz",   0x85, 0, ENC_J, 0 },
    { "jg",    0x8F, 0, ENC_J, 0 },    { "jge",   0x8D, 0, ENC_J, 0 },
    { "jl",    0x8C, 0, ENC_J, 0 },    { "jle",   0x8E, 0, ENC_J, 0 },
    { "ja",    0x87, 0, ENC_J, 0 },    { "jae",   0x83, 0, ENC_J, 0 },
    { "jb",    0x82, 0, ENC_J, 0 },    { "jbe",   0x86, 0, ENC_J, 0 },
    { "loop",  0xE2, 0, ENC_J, 0 },

    { "mov",   0x88, 0, ENC_MR, 1 },   { "mov",   0x8A, 0, ENC_RM, 1 },
    { "mov",   0xB0, 0, ENC_OI, 1 },   { "mov",   0xC6, 0, ENC_MI, 1 },

    { "mov",   0x89, 0, ENC_MR, 4 },   { "mov",   0x8B, 0, ENC_RM, 4 },
    { "mov",   0xB8, 0, ENC_OI, 4 },   { "mov",   0xC7, 0, ENC_MI, 4 },
    { "lea",   0x8D, 0, ENC_RM, 4 },

    { "xchg",  0x86, 0, ENC_MR, 1 },   { "xchg",  0x87, 0, ENC_MR, 4 },
    { "xchg",  0x90, 0, ENC_R,  4 },

    { "movzx", 0xB6, 0, ENC_0F_MR, 1 }, { "movzx", 0xB7, 0, ENC_0F_MR, 4 },
    { "movsx", 0xBE, 0, ENC_0F_MR, 1 }, { "movsx", 0xBF, 0, ENC_0F_MR, 4 },

    { "bt",    0xA3, 0, ENC_0F_MR, 4 }, { "bts",   0xAB, 0, ENC_0F_MR, 4 },
    { "btr",   0xB3, 0, ENC_0F_MR, 4 }, { "btc",   0xBB, 0, ENC_0F_MR, 4 },

    { "bsf",   0xBC, 0, ENC_0F_RM, 4 }, { "bsr",   0xBD, 0, ENC_0F_RM, 4 },

    { "movb",  0xC6, 0, ENC_MI, 1 },   { "movb",  0x88, 0, ENC_MR, 1 },   { "movb",  0x8A, 0, ENC_RM, 1 },

    { "add",   0x00, 0, ENC_MR, 1 }, { "add",   0x02, 0, ENC_RM, 1 }, { "add",   0x80, 0, ENC_MI, 1 },
    { "or",    0x08, 0, ENC_MR, 1 }, { "or",    0x0A, 0, ENC_RM, 1 }, { "or",    0x80, 1, ENC_MI, 1 },
    { "adc",   0x10, 0, ENC_MR, 1 }, { "adc",   0x12, 0, ENC_RM, 1 }, { "adc",   0x80, 2, ENC_MI, 1 },
    { "sbb",   0x18, 0, ENC_MR, 1 }, { "sbb",   0x1A, 0, ENC_RM, 1 }, { "sbb",   0x80, 3, ENC_MI, 1 },
    { "and",   0x20, 0, ENC_MR, 1 }, { "and",   0x22, 0, ENC_RM, 1 }, { "and",   0x80, 4, ENC_MI, 1 },
    { "sub",   0x28, 0, ENC_MR, 1 }, { "sub",   0x2A, 0, ENC_RM, 1 }, { "sub",   0x80, 5, ENC_MI, 1 },
    { "xor",   0x30, 0, ENC_MR, 1 }, { "xor",   0x32, 0, ENC_RM, 1 }, { "xor",   0x80, 6, ENC_MI, 1 },
    { "cmp",   0x38, 0, ENC_MR, 1 }, { "cmp",   0x3A, 0, ENC_RM, 1 }, { "cmp",   0x80, 7, ENC_MI, 1 },
    { "test",  0x84, 0, ENC_MR, 1 }, { "test",  0xF6, 0, ENC_MI, 1 },

    { "add",   0x01, 0, ENC_MR, 4 }, { "add",   0x03, 0, ENC_RM, 4 }, { "add",   0x81, 0, ENC_MI, 4 }, { "add", 0x83, 0, ENC_MI, 4 },
    { "or",    0x09, 0, ENC_MR, 4 }, { "or",    0x0B, 0, ENC_RM, 4 }, { "or",    0x81, 1, ENC_MI, 4 }, { "or",  0x83, 1, ENC_MI, 4 },
    { "adc",   0x11, 0, ENC_MR, 4 }, { "adc",   0x13, 0, ENC_RM, 4 }, { "adc",   0x81, 2, ENC_MI, 4 }, { "adc", 0x83, 2, ENC_MI, 4 },
    { "sbb",   0x19, 0, ENC_MR, 4 }, { "sbb",   0x1B, 0, ENC_RM, 4 }, { "sbb",   0x81, 3, ENC_MI, 4 }, { "sbb", 0x83, 3, ENC_MI, 4 },
    { "and",   0x21, 0, ENC_MR, 4 }, { "and",   0x23, 0, ENC_RM, 4 }, { "and",   0x81, 4, ENC_MI, 4 }, { "and", 0x83, 4, ENC_MI, 4 },
    { "sub",   0x29, 0, ENC_MR, 4 }, { "sub",   0x2B, 0, ENC_RM, 4 }, { "sub",   0x81, 5, ENC_MI, 4 }, { "sub", 0x83, 5, ENC_MI, 4 },
    { "xor",   0x31, 0, ENC_MR, 4 }, { "xor",   0x33, 0, ENC_RM, 4 }, { "xor",   0x81, 6, ENC_MI, 4 }, { "xor", 0x83, 6, ENC_MI, 4 },
    { "cmp",   0x39, 0, ENC_MR, 4 }, { "cmp",   0x3B, 0, ENC_RM, 4 }, { "cmp",   0x81, 7, ENC_MI, 4 }, { "cmp", 0x83, 7, ENC_MI, 4 },
    { "test",  0x85, 0, ENC_MR, 4 }, { "test",  0xF7, 0, ENC_MI, 4 },

    { "inc",   0x40, 0, ENC_R, 2 },    { "dec",   0x48, 0, ENC_R, 2 },

    { "seto",   0x90, 0, ENC_0F_MR, 1 }, { "setno",  0x91, 0, ENC_0F_MR, 1 },
    { "setb",   0x92, 0, ENC_0F_MR, 1 }, { "setnae", 0x92, 0, ENC_0F_MR, 1 }, { "setc",   0x92, 0, ENC_0F_MR, 1 },
    { "setae",  0x93, 0, ENC_0F_MR, 1 }, { "setnb",  0x93, 0, ENC_0F_MR, 1 }, { "setnc",  0x93, 0, ENC_0F_MR, 1 },
    { "sete",   0x94, 0, ENC_0F_MR, 1 }, { "setz",   0x94, 0, ENC_0F_MR, 1 },
    { "setne",  0x95, 0, ENC_0F_MR, 1 }, { "setnz",  0x95, 0, ENC_0F_MR, 1 },
    { "setbe",  0x96, 0, ENC_0F_MR, 1 }, { "setna",  0x96, 0, ENC_0F_MR, 1 },
    { "seta",   0x97, 0, ENC_0F_MR, 1 }, { "setnbe", 0x97, 0, ENC_0F_MR, 1 },
    { "sets",   0x98, 0, ENC_0F_MR, 1 }, { "setns",  0x99, 0, ENC_0F_MR, 1 },
    { "setp",   0x9A, 0, ENC_0F_MR, 1 }, { "setpe",  0x9A, 0, ENC_0F_MR, 1 },
    { "setnp",  0x9B, 0, ENC_0F_MR, 1 }, { "setpo",  0x9B, 0, ENC_0F_MR, 1 },
    { "setl",   0x9C, 0, ENC_0F_MR, 1 }, { "setnge", 0x9C, 0, ENC_0F_MR, 1 },
    { "setge",  0x9D, 0, ENC_0F_MR, 1 }, { "setnl",  0x9D, 0, ENC_0F_MR, 1 },
    { "setle",  0x9E, 0, ENC_0F_MR, 1 }, { "setng",  0x9E, 0, ENC_0F_MR, 1 },
    { "setg",   0x9F, 0, ENC_0F_MR, 1 }, { "setnle", 0x9F, 0, ENC_0F_MR, 1 },

    { "cmovo",   0x40, 0, ENC_0F_RM, 4 }, { "cmovno",  0x41, 0, ENC_0F_RM, 4 },
    { "cmovb",   0x42, 0, ENC_0F_RM, 4 }, { "cmovnae", 0x42, 0, ENC_0F_RM, 4 }, { "cmovc",   0x42, 0, ENC_0F_RM, 4 },
    { "cmovae",  0x43, 0, ENC_0F_RM, 4 }, { "cmovnb",  0x43, 0, ENC_0F_RM, 4 }, { "cmovnc",  0x43, 0, ENC_0F_RM, 4 },
    { "cmove",   0x44, 0, ENC_0F_RM, 4 }, { "cmovz",   0x44, 0, ENC_0F_RM, 4 },
    { "cmovne",  0x45, 0, ENC_0F_RM, 4 }, { "cmovnz",  0x45, 0, ENC_0F_RM, 4 },
    { "cmovbe",  0x46, 0, ENC_0F_RM, 4 }, { "cmovna",  0x46, 0, ENC_0F_RM, 4 },
    { "cmova",   0x47, 0, ENC_0F_RM, 4 }, { "cmovnbe", 0x47, 0, ENC_0F_RM, 4 },
    { "cmovs",   0x48, 0, ENC_0F_RM, 4 }, { "cmovns",  0x49, 0, ENC_0F_RM, 4 },
    { "cmovp",   0x4A, 0, ENC_0F_RM, 4 }, { "cmovpe",  0x4A, 0, ENC_0F_RM, 4 },
    { "cmovnp",  0x4B, 0, ENC_0F_RM, 4 }, { "cmovpo",  0x4B, 0, ENC_0F_RM, 4 },
    { "cmovl",   0x4C, 0, ENC_0F_RM, 4 }, { "cmovnge", 0x4C, 0, ENC_0F_RM, 4 },
    { "cmovge",  0x4D, 0, ENC_0F_RM, 4 }, { "cmovnl",  0x4D, 0, ENC_0F_RM, 4 },
    { "cmovle",  0x4E, 0, ENC_0F_RM, 4 }, { "cmovng",  0x4E, 0, ENC_0F_RM, 4 },
    { "cmovg",   0x4F, 0, ENC_0F_RM, 4 }, { "cmovnle", 0x4F, 0, ENC_0F_RM, 4 },

    { "shl",   0xC1, 4, ENC_SHIFT, 4 }, { "shr",   0xC1, 5, ENC_SHIFT, 4 },
    { "sal",   0xC1, 4, ENC_SHIFT, 4 }, { "sar",   0xC1, 7, ENC_SHIFT, 4 },
    { "rol",   0xC1, 0, ENC_SHIFT, 4 }, { "ror",   0xC1, 1, ENC_SHIFT, 4 },
    { "shl",   0xD1, 4, ENC_SHIFT, 4 }, { "shr",   0xD1, 5, ENC_SHIFT, 4 },
    { "sal",   0xD1, 4, ENC_SHIFT, 4 }, { "sar",   0xD1, 7, ENC_SHIFT, 4 },

    { 0,0,0,0,0 }
};

static int* isa_bucket_head;
static int* isa_bucket_tail;
static int* isa_next;
static int isa_bucket_size;
static int isa_count;
static int isa_index_built;

static uint32_t isa_hash_calc(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

void isa_build_index(void) {
    if (isa_index_built) return;

    isa_count = 0;
    while (isa[isa_count].mnem) isa_count++;

    isa_bucket_size = 1;
    while (isa_bucket_size < isa_count * 2) isa_bucket_size <<= 1;

    isa_bucket_head = (int*)malloc(sizeof(int) * (size_t)isa_bucket_size);
    isa_bucket_tail = (int*)malloc(sizeof(int) * (size_t)isa_bucket_size);
    isa_next = (int*)malloc(sizeof(int) * (size_t)isa_count);
    if (!isa_bucket_head || !isa_bucket_tail || !isa_next) exit(1);

    for (int i = 0; i < isa_bucket_size; i++) {
        isa_bucket_head[i] = -1;
        isa_bucket_tail[i] = -1;
    }

    for (int i = 0; i < isa_count; i++) {
        const char* m = isa[i].mnem;
        if (!m) continue;
        uint32_t h = isa_hash_calc(m);
        int slot = (int)(h & (uint32_t)(isa_bucket_size - 1));
        isa_next[i] = -1;
        if (isa_bucket_head[slot] == -1) {
            isa_bucket_head[slot] = i;
            isa_bucket_tail[slot] = i;
        } else {
            int tail = isa_bucket_tail[slot];
            isa_next[tail] = i;
            isa_bucket_tail[slot] = i;
        }
    }

    isa_index_built = 1;
}

void isa_free_index(void) {
    if (isa_bucket_head) free(isa_bucket_head);
    if (isa_bucket_tail) free(isa_bucket_tail);
    if (isa_next) free(isa_next);
    isa_bucket_head = isa_bucket_tail = isa_next = 0;
    isa_bucket_size = 0;
    isa_count = 0;
    isa_index_built = 0;
}

void parse_operand(AssemblerCtx* ctx, char* text, Operand* op) {
    memset(op, 0, sizeof(Operand));
    op->reg = -1;
    op->size = 0;
    op->base_reg = -1;
    op->index_reg = -1;
    op->scale = 1;
    if (!text || !*text) { op->type = OP_NONE; return; }

    if (text[0] == '[') {
        op->type = OP_MEM;
        int len = strlen(text);
        if (text[len-1] != ']') panic(ctx, "Missing ']'");
        text[len-1] = 0;
        char* content = text + 1;

        if (ctx->code16) {
            char* p = content;
            int sign = 1;

            while (*p == ' ' || *p == '\t') p++;
            if (*p == '+' || *p == '-') {
                sign = (*p == '-') ? -1 : 1;
                p++;
            }

            while (1) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p == 0) break;

                char* t = p;
                while (*p && *p != '+' && *p != '-') p++;
                int term_len = (int)(p - t);
                while (term_len > 0 && (t[term_len-1] == ' ' || t[term_len-1] == '\t')) term_len--;
                if (term_len <= 0) {
                    if (*p == '+' || *p == '-') {
                        sign = (*p == '-') ? -1 : 1;
                        p++;
                        continue;
                    }
                    break;
                }

                char tmp[64];
                int n = term_len;
                if (n > 63) n = 63;
                memcpy(tmp, t, n);
                tmp[n] = 0;

                int sz;
                int r = get_reg_info(tmp, &sz);
                if (r != -1) {
                    if (sz != 2) panic(ctx, "Only 16-bit registers allowed in use16");
                    if (!is_16bit_addr_reg(r)) panic(ctx, "Only BX,BP,SI,DI allowed in 16-bit memory address");
                    if (sign < 0) panic(ctx, "Negative register not supported");
                    if (op->base_reg == -1) op->base_reg = r;
                    else if (op->index_reg == -1) op->index_reg = r;
                    else panic(ctx, "Too many registers in 16-bit memory address");
                } else if ((tmp[0] >= '0' && tmp[0] <= '9') || tmp[0] == '-' || tmp[0] == '(') {
                    int val = eval_number(ctx, tmp);
                    op->disp += sign * val;
                } else {
                    if (op->has_label) panic(ctx, "Multiple labels in memory operand");
                    if (op->base_reg != -1 || op->index_reg != -1) panic(ctx, "Labels with registers not supported in 16-bit memory operand");
                    if (sign < 0) panic(ctx, "Negative label not supported");
                    op->has_label = 1;
                    char full[64];
                    resolve_symbol_name(ctx, tmp, full, sizeof(full));
                    strcpy(op->label, full);
                }

                if (*p == '+' || *p == '-') {
                    sign = (*p == '-') ? -1 : 1;
                    p++;
                } else {
                    break;
                }
            }

            if (op->base_reg != -1) op->reg = op->base_reg;
            return;
        }

        char* p = content;
        int sign = 1;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '+' || *p == '-') {
            sign = (*p == '-') ? -1 : 1;
            p++;
        }

        while (1) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 0) break;

            char* t = p;
            while (*p && *p != '+' && *p != '-') p++;
            int term_len = (int)(p - t);
            while (term_len > 0 && (t[term_len-1] == ' ' || t[term_len-1] == '\t')) term_len--;
            if (term_len <= 0) {
                if (*p == '+' || *p == '-') {
                    sign = (*p == '-') ? -1 : 1;
                    p++;
                    continue;
                }
                break;
            }

            char tmp[64];
            int n = term_len;
            if (n > 63) n = 63;
            memcpy(tmp, t, n);
            tmp[n] = 0;

            char* star = 0;
            for (char* q = tmp; *q; q++) {
                if (*q == '*') { star = q; break; }
            }

            if (star) {
                *star = 0;
                char* left = tmp;
                char* right = star + 1;

                while (*left == ' ' || *left == '\t') left++;
                char* end = left + strlen(left);
                while (end > left && (end[-1] == ' ' || end[-1] == '\t')) { *--end = 0; }

                while (*right == ' ' || *right == '\t') right++;
                end = right + strlen(right);
                while (end > right && (end[-1] == ' ' || end[-1] == '\t')) { *--end = 0; }

                int sz;
                int r = get_reg_info(left, &sz);
                if (r == -1 || sz != 4) panic(ctx, "Index register must be 32-bit");
                if (sign < 0) panic(ctx, "Negative scaled index not supported");
                int sc = eval_number(ctx, right);
                if (sc != 1 && sc != 2 && sc != 4 && sc != 8) panic(ctx, "Scale must be 1,2,4 or 8");
                if (op->index_reg != -1) panic(ctx, "Multiple index registers");
                op->index_reg = r;
                op->scale = sc;
            } else {
                int sz;
                int r = get_reg_info(tmp, &sz);
                if (r != -1) {
                    if (sz != 4) panic(ctx, "Memory register must be 32-bit");
                    if (sign < 0) panic(ctx, "Negative register not supported");
                    if (op->base_reg == -1) op->base_reg = r;
                    else if (op->index_reg == -1) { op->index_reg = r; op->scale = 1; }
                    else panic(ctx, "Too many registers in memory operand");
                } else if ((tmp[0] >= '0' && tmp[0] <= '9') || tmp[0] == '-' || tmp[0] == '(') {
                    int val = eval_number(ctx, tmp);
                    op->disp += sign * val;
                } else {
                    if (op->has_label) panic(ctx, "Multiple labels in memory operand");
                    if (op->base_reg != -1 || op->index_reg != -1) panic(ctx, "Labels with registers not supported in memory operand");
                    if (sign < 0) panic(ctx, "Negative label not supported");
                    op->has_label = 1;
                    char full[64];
                    resolve_symbol_name(ctx, tmp, full, sizeof(full));
                    strcpy(op->label, full);
                }

                if (*p == '+' || *p == '-') {
                    sign = (*p == '-') ? -1 : 1;
                    p++;
                } else {
                    break;
                }
            }

        }

        if (op->base_reg != -1) op->reg = op->base_reg;
        return;
    }

    int sz;
    int r = get_reg_info(text, &sz);
    if (r != -1) {
        op->type = OP_REG;
        op->reg = r;
        op->size = sz;
        return;
    }

    op->type = OP_IMM;

    if (text[0] == '\'' && text[2] == '\'' && text[1] != 0) {
        op->disp = (int)text[1];
        op->size = 1;
        return;
    }

    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-') {
        op->disp = eval_number(ctx, text);
        if (op->disp >= -128 && op->disp <= 255) op->size = 1;
        else op->size = 4;
    } else {
        char full[64];
        resolve_symbol_name(ctx, text, full, sizeof(full));

        Symbol* s = sym_find(ctx, full);
        if (s && s->section == SEC_ABS) {
            op->disp = s->value;
            if (op->disp >= -128 && op->disp <= 255) op->size = 1;
            else op->size = 4;
        } else {
            strcpy(op->label, full);
            op->has_label = 1;
            op->size = 4;
        }
    }
}

Buffer* get_cur_buffer(AssemblerCtx* ctx) {
    if (ctx->cur_sec == SEC_DATA) return &ctx->data;
    if (ctx->cur_sec == SEC_BSS) return &ctx->bss;
    return &ctx->text;
}

static void emit_byte(AssemblerCtx* ctx, uint8_t b) {
    Buffer* buf = get_cur_buffer(ctx);
    if (ctx->pass == 1) { buf->size++; return; }
    buf_push(buf, b);
}

static void emit_word(AssemblerCtx* ctx, uint16_t w) {
    Buffer* buf = get_cur_buffer(ctx);
    if (ctx->pass == 1) { buf->size += 2; return; }
    buf_push(buf, w & 0xFF);
    buf_push(buf, (w >> 8) & 0xFF);
}

static void emit_dword(AssemblerCtx* ctx, uint32_t d) {
    Buffer* buf = get_cur_buffer(ctx);
    if (ctx->pass == 1) { buf->size += 4; return; }
    buf_push_u32(buf, d);
}

void emit_reloc(AssemblerCtx* ctx, int type, char* label, uint32_t offset) {
    if (ctx->pass != 2) return;
    Symbol* s = sym_find(ctx, label);
    if (!s) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Undefined symbol '%s'", label);
        panic(ctx, buf);
    }

    Elf32_Rel r;
    r.r_offset = offset;
    r.r_info = ELF32_R_INFO(s->elf_idx, type);

    Buffer* target = (ctx->cur_sec == SEC_TEXT) ? &ctx->rel_text : &ctx->rel_data;
    buf_write(target, &r, sizeof(r));
}

static void emit_modrm16(AssemblerCtx* ctx, int reg_opcode, Operand* rm) {
    if (ctx->pass != 2) return;

    int base = rm->base_reg;
    int index = rm->index_reg;
    int32_t disp = rm->disp;

    if (base == -1 && index == -1) {
        uint16_t val = (uint16_t)disp;
        if (rm->has_label) {
            if (ctx->format != FMT_BIN) {
                panic(ctx, "16-bit relocations in ELF are not supported");
            }
            Symbol* s = sym_find(ctx, rm->label);
            if (!s) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Undefined symbol '%s'", rm->label);
                panic(ctx, msg);
            }
            val = (uint16_t)resolve_abs_addr(ctx, s);
        }
        emit_byte(ctx, (0 << 6) | ((reg_opcode & 7) << 3) | 6);
        emit_word(ctx, val);
        return;
    }

    int has_bx = 0, has_bp = 0, has_si = 0, has_di = 0;
    int regs[2] = { base, index };
    for (int i = 0; i < 2; i++) {
        int r = regs[i];
        if (r == -1) continue;
        if (r == 3) has_bx = 1;
        else if (r == 5) has_bp = 1;
        else if (r == 6) has_si = 1;
        else if (r == 7) has_di = 1;
        else {
            panic(ctx, "Invalid register in 16-bit address");
        }
    }

    int rm_bits = -1;
    if (has_bx && has_si && !has_bp && !has_di) rm_bits = 0;
    else if (has_bx && has_di && !has_bp && !has_si) rm_bits = 1;
    else if (has_bp && has_si && !has_bx && !has_di) rm_bits = 2;
    else if (has_bp && has_di && !has_bx && !has_si) rm_bits = 3;
    else if (has_si && !has_bx && !has_bp && !has_di) rm_bits = 4;
    else if (has_di && !has_bx && !has_bp && !has_si) rm_bits = 5;
    else if (has_bp && !has_bx && !has_si && !has_di) rm_bits = 6;
    else if (has_bx && !has_bp && !has_si && !has_di) rm_bits = 7;
    else {
        panic(ctx, "Unsupported 16-bit addressing combination");
    }

    uint8_t mod;
    uint16_t disp16 = (uint16_t)disp;

    if (!rm->has_label) {
        if (disp == 0 && rm_bits != 6) {
            mod = 0;
        } else if (disp >= -128 && disp <= 127) {
            mod = 1;
        } else {
            mod = 2;
        }
    } else {
        if (ctx->format != FMT_BIN) {
            panic(ctx, "16-bit relocations in ELF are not supported");
        }
        Symbol* s = sym_find(ctx, rm->label);
        if (!s) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Undefined symbol '%s'", rm->label);
            panic(ctx, msg);
        }
        disp16 = (uint16_t)resolve_abs_addr(ctx, s);
        mod = 2;
    }

    emit_byte(ctx, (mod << 6) | ((reg_opcode & 7) << 3) | rm_bits);

    if (mod == 1) {
        emit_byte(ctx, (uint8_t)disp);
    } else if (mod == 2 || (mod == 0 && rm_bits == 6)) {
        emit_word(ctx, disp16);
    }
}

static void emit_modrm(AssemblerCtx* ctx, int reg_opcode, Operand* rm) {
    if (ctx->pass != 2) return;

    Buffer* buf = get_cur_buffer(ctx);

    if (rm->type == OP_REG) {
        emit_byte(ctx, 0xC0 | ((reg_opcode & 7) << 3) | (rm->reg & 7));
    } else {
        if (ctx->code16) {
            emit_modrm16(ctx, reg_opcode, rm);
            return;
        }

        int base = rm->base_reg;
        int index = rm->index_reg;
        int32_t disp = rm->disp;

        if (base == -1 && index == -1) {
            emit_byte(ctx, (0 << 6) | ((reg_opcode & 7) << 3) | 5);
            uint32_t val = (uint32_t)disp;
            if (rm->has_label) {
                if (ctx->format == FMT_BIN) {
                    Symbol* s = sym_find(ctx, rm->label);
                    if (!s) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Undefined symbol '%s'", rm->label);
                        panic(ctx, msg);
                    }
                    val = resolve_abs_addr(ctx, s);
                } else {
                    emit_reloc(ctx, R_386_32, rm->label, buf->size);
                    val = 0;
                }
            }
            emit_dword(ctx, val);
            return;
        }

        int use_sib = 0;
        if (index != -1 || base == 4) use_sib = 1;

        if (!use_sib) {
            uint8_t mod;
            uint8_t rm_bits = (uint8_t)(base & 7);

            if (disp == 0 && base != 5) {
                mod = 0;
            } else if (disp >= -128 && disp <= 127) {
                mod = 1;
            } else {
                mod = 2;
            }

            emit_byte(ctx, (mod << 6) | ((reg_opcode & 7) << 3) | rm_bits);

            if (mod == 1) {
                emit_byte(ctx, (uint8_t)disp);
            } else if (mod == 2 || (mod == 0 && base == 5)) {
                uint32_t val = (uint32_t)disp;
                emit_dword(ctx, val);
            }
            return;
        }

        int scale_bits = 0;
        if (rm->scale == 1) scale_bits = 0;
        else if (rm->scale == 2) scale_bits = 1;
        else if (rm->scale == 4) scale_bits = 2;
        else if (rm->scale == 8) scale_bits = 3;

        int index_bits = (index == -1) ? 4 : (index & 7);
        int base_bits;
        uint8_t mod;

        if (base == -1) {
            base_bits = 5;
            mod = 0;
            emit_byte(ctx, (mod << 6) | ((reg_opcode & 7) << 3) | 4);
            emit_byte(ctx, (uint8_t)((scale_bits << 6) | (index_bits << 3) | base_bits));
            uint32_t val = (uint32_t)disp;
            emit_dword(ctx, val);
            return;
        }

        if (disp == 0 && base != 5) {
            mod = 0;
        } else if (disp >= -128 && disp <= 127) {
            mod = 1;
        } else {
            mod = 2;
        }

        base_bits = base & 7;

        emit_byte(ctx, (mod << 6) | ((reg_opcode & 7) << 3) | 4);
        emit_byte(ctx, (uint8_t)((scale_bits << 6) | (index_bits << 3) | base_bits));

        if (mod == 1) {
            emit_byte(ctx, (uint8_t)disp);
        } else if (mod == 2 || (mod == 0 && base_bits == 5)) {
            uint32_t val = (uint32_t)disp;
            emit_dword(ctx, val);
        }
    }
}

void assemble_instr(AssemblerCtx* ctx, char* name, int explicit_size, Operand* o1, Operand* o2) {
    int size = explicit_size;
    if (size == 0) {
        if (o1->type == OP_REG) size = o1->size;
        else if (o2->type == OP_REG) size = o2->size;
    }
    if (size == 0) size = ctx->default_size ? ctx->default_size : 4;

    if (size == 2) {
        if (!ctx->code16) emit_byte(ctx, 0x66);
    } else if (size == 4) {
        if (ctx->code16) emit_byte(ctx, 0x66);
    }

    uint32_t h = isa_hash_calc(name);
    int slot = (int)(h & (uint32_t)(isa_bucket_size - 1));

    for (int i = isa_bucket_head[slot]; i != -1; i = isa_next[i]) {
        InstrDef* d = &isa[i];
        if (strcmp(d->mnem, name) != 0) continue;

        int match_size = d->size;
        if (match_size == 4 && size == 2) match_size = 2;

        if (d->size != 0 && match_size != size) continue;

        if (d->mode == ENC_NONE) {
            if (o1->type != OP_NONE) continue;
            emit_byte(ctx, d->op_base);
            return;
        }
        if (d->mode == ENC_0F) {
            if (o1->type != OP_NONE) continue;
            emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base);
            return;
        }
        if (d->mode == ENC_0F_MR) {
            if (o2->type != OP_REG || o1->type == OP_IMM) continue;
            emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base);
            emit_modrm(ctx, o2->reg, o1);
            return;
        }
        if (d->mode == ENC_0F_RM) {
            if (o1->type != OP_REG || o2->type == OP_IMM) continue;
            emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base);
            emit_modrm(ctx, o1->reg, o2);
            return;
        }
        if (d->mode == ENC_R) {
            if (o1->type != OP_REG) continue;
            emit_byte(ctx, d->op_base + o1->reg);
            return;
        }
        if (d->mode == ENC_I) {
            if (o1->type != OP_IMM) continue;
            if (d->op_base == 0xCD) { emit_byte(ctx, d->op_base); emit_byte(ctx, (uint8_t)o1->disp); }
            else {
                emit_byte(ctx, d->op_base);
                uint32_t val = o1->disp;
                Buffer* buf = get_cur_buffer(ctx);
                if (o1->has_label) {
                    if (ctx->format == FMT_BIN) {
                        Symbol* s = sym_find(ctx, o1->label);
                        if (!s) {
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Undefined symbol '%s'", o1->label);
                            panic(ctx, msg);
                        }
                        val = resolve_abs_addr(ctx, s);
                    } else {
                        emit_reloc(ctx, R_386_32, o1->label, buf->size);
                        val = 0;
                    }
                }

                if (size == 2) emit_word(ctx, (uint16_t)val);
                else emit_dword(ctx, val);
            }
            return;
        }
        if (d->mode == ENC_J) {
            if (o1->type != OP_IMM) continue;
            Buffer* buf = get_cur_buffer(ctx);

            if (d->op_base == 0xE2) {
                 emit_byte(ctx, d->op_base);
                 int32_t delta = -2;
                 if (ctx->pass == 2 && o1->has_label) {
                     Symbol* s = sym_find(ctx, o1->label);
                     if (s && s->section == ctx->cur_sec) delta = s->value - (buf->size + 1);
                 }
                 emit_byte(ctx, (int8_t)delta);
                 return;
            }

            if (d->op_base >= 0x80 && d->op_base <= 0x8F) emit_byte(ctx, 0x0F);
            emit_byte(ctx, d->op_base);

            uint32_t val = 0;
            if (ctx->pass == 2) {
                if (o1->has_label) {
                    if (ctx->format == FMT_BIN) {
                        Symbol* s = sym_find(ctx, o1->label);
                        if (!s) {
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Undefined symbol '%s'", o1->label);
                            panic(ctx, msg);
                        }
                        if (s->section != ctx->cur_sec) {
                            panic(ctx, "PC-relative jump across sections not supported in binary format");
                        }
                        int32_t target = (int32_t)s->value;
                        int32_t pc = (int32_t)(buf->size + 4);
                        val = (uint32_t)(target - pc);
                    } else {
                        emit_reloc(ctx, R_386_PC32, o1->label, buf->size);
                        val = (uint32_t)-4;
                    }
                } else {
                    val = (uint32_t)o1->disp;
                }
            }
            emit_dword(ctx, val);
            return;
        }
        if (d->mode == ENC_OI) {
            if (o1->type != OP_REG || o2->type != OP_IMM) continue;
            emit_byte(ctx, d->op_base + o1->reg);
            uint32_t val = o2->disp;
            Buffer* buf = get_cur_buffer(ctx);
            if (o2->has_label && ctx->pass == 2) {
                if (ctx->format == FMT_BIN) {
                    Symbol* s = sym_find(ctx, o2->label);
                    if (!s) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Undefined symbol '%s'", o2->label);
                        panic(ctx, msg);
                    }
                    val = resolve_abs_addr(ctx, s);
                } else {
                    emit_reloc(ctx, R_386_32, o2->label, buf->size);
                    val = 0;
                }
            }

            if (size == 1) emit_byte(ctx, (uint8_t)val);
            else if (size == 2) emit_word(ctx, (uint16_t)val);
            else emit_dword(ctx, val);
            return;
        }
        if (d->mode == ENC_MR) {
            if (o2->type != OP_REG || o1->type == OP_IMM) continue;
            emit_byte(ctx, d->op_base); emit_modrm(ctx, o2->reg, o1);
            return;
        }
        if (d->mode == ENC_RM) {
            if (o1->type != OP_REG || o2->type == OP_IMM) continue;
            emit_byte(ctx, d->op_base); emit_modrm(ctx, o1->reg, o2);
            return;
        }
        if (d->mode == ENC_MI) {
            if (o2->type != OP_IMM || o1->type == OP_IMM) continue;
            if (d->op_base == 0x83) {
                if (o2->disp < -128 || o2->disp > 127) continue;
            }

            emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1);

            if (size == 1 || d->op_base == 0x83) {
                emit_byte(ctx, (uint8_t)o2->disp);
            } else {
                if (size == 2) emit_word(ctx, (uint16_t)o2->disp);
                else emit_dword(ctx, o2->disp);
            }
            return;
        }
        if (d->mode == ENC_M) {
            if (o1->type == OP_IMM || o2->type != OP_NONE) continue;
            emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1);
            return;
        }
        if (d->mode == ENC_SHIFT) {
            if (o1->type == OP_IMM || o2->type != OP_IMM) continue;
            if (d->op_base == 0xD1 || d->op_base == 0xD0) {
                if (o2->disp != 1) continue;
                emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1);
                return;
            }
            emit_byte(ctx, d->op_base); emit_modrm(ctx, d->op_ext, o1); emit_byte(ctx, (uint8_t)o2->disp);
            return;
        }
    }

    {
        char buf[128];
        snprintf(buf, sizeof(buf), "Unknown instruction '%s'", name);
        panic(ctx, buf);
    }
}

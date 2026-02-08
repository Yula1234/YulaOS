// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "geditor_defs.h"

int WIN_W = 800;
int WIN_H = 600;

uint32_t* canvas;
Editor ed;

const uint32_t surface_id = 1u;

comp_conn_t conn;
char shm_name[32];
int shm_fd = -1;
int shm_gen = 0;
uint32_t size_bytes = 0;

const char* kwd_general[] = {
    "mov", "lea", "push", "pop", "add", "sub", "imul", "div", "xor", "or", "and",
    "cmp", "test", "inc", "dec", "hlt", "cli", "sti", "nop", "int", "shl", "shr",
    "rol", "ror", "neg", "not", 0
};

const char* kwd_control[] = {
    "jmp", "je", "jne", "jg", "jge", "jl", "jle", "jz", "jnz", "call", "ret", "loop",
    "ja", "jb", "jae", "jbe", 0
};

const char* kwd_dirs[] = {
    "section", "global", "extern", "public", "db", "dw", "dd", "dq", "rb", "resb",
    "use32", "format", "org", "entry", "byte", "word", "dword", "ptr", "equ", 0
};

const char* kwd_regs[] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp", "ax", "bx", "cx", "dx",
    "al", "ah", "bl", "bh", "dl", "dh", "cl", "ch", 0
};

const char* c_kwd_types[] = {
    "void", "char", "short", "int", "long", "signed", "unsigned", "float", "double",
    "struct", "union", "enum", "typedef", "const", "volatile", "static", "extern",
    "register", "auto", "inline", "sizeof", 0
};

const char* c_kwd_ctrl[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", 0
};

const char* c_kwd_pp[] = {
    "include", "define", "undef", "ifdef", "ifndef", "if", "elif", "else",
    "endif", "error", "pragma", 0
};

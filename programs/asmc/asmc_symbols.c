// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_symbols.h"

static uint32_t sym_hash_calc(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static void sym_hash_rebuild(AssemblerCtx* ctx, int new_size) {
    if (new_size < 32) new_size = 32;
    int p = 1;
    while (p < new_size) p <<= 1;
    new_size = p;

    int* new_hash = (int*)malloc(sizeof(int) * new_size);
    if (!new_hash) exit(1);
    for (int i = 0; i < new_size; i++) new_hash[i] = -1;

    for (int i = 0; i < ctx->sym_count; i++) {
        uint32_t h = sym_hash_calc(ctx->symbols[i].name);
        int mask = new_size - 1;
        for (int step = 0; step < new_size; step++) {
            int slot = (int)((h + (uint32_t)step) & (uint32_t)mask);
            if (new_hash[slot] == -1) {
                new_hash[slot] = i;
                break;
            }
        }
    }

    if (ctx->sym_hash) free(ctx->sym_hash);
    ctx->sym_hash = new_hash;
    ctx->sym_hash_size = new_size;
}

static void sym_hash_insert(AssemblerCtx* ctx, int index) {
    if (ctx->sym_hash_size == 0 || !ctx->sym_hash) {
        sym_hash_rebuild(ctx, 256);
    } else if (ctx->sym_count * 4 >= ctx->sym_hash_size * 3) {
        sym_hash_rebuild(ctx, ctx->sym_hash_size * 2);
    }

    uint32_t h = sym_hash_calc(ctx->symbols[index].name);
    int mask = ctx->sym_hash_size - 1;
    for (int step = 0; step < ctx->sym_hash_size; step++) {
        int slot = (int)((h + (uint32_t)step) & (uint32_t)mask);
        if (ctx->sym_hash[slot] == -1) {
            ctx->sym_hash[slot] = index;
            return;
        }
    }
}

void sym_table_init(AssemblerCtx* ctx) {
    ctx->sym_capacity = 256;
    ctx->symbols = (Symbol*)malloc(sizeof(Symbol) * (size_t)ctx->sym_capacity);
    if (!ctx->symbols) exit(1);
    ctx->sym_count = 0;
    ctx->sym_hash = 0;
    ctx->sym_hash_size = 0;
}

void sym_table_free(AssemblerCtx* ctx) {
    if (ctx->symbols) free(ctx->symbols);
    if (ctx->sym_hash) free(ctx->sym_hash);
    ctx->symbols = 0;
    ctx->sym_hash = 0;
    ctx->sym_capacity = 0;
    ctx->sym_hash_size = 0;
    ctx->sym_count = 0;
}

void normalize_symbol_name(AssemblerCtx* ctx, const char* in, char* out, size_t out_size) {
    if (!in || out_size == 0) return;
    if (in[0] == '.') {
        if (!ctx->current_scope[0]) {
            panic(ctx, "Local label without global label");
        }
        size_t base_len = strlen(ctx->current_scope);
        size_t local_len = strlen(in + 1);
        if (base_len + 1 + local_len >= out_size) {
            panic(ctx, "Symbol name too long");
        }
        memcpy(out, ctx->current_scope, base_len);
        out[base_len] = '$';
        memcpy(out + base_len + 1, in + 1, local_len + 1);
    } else {
        size_t len = strlen(in);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, in, len);
        out[len] = 0;
    }
}

void resolve_symbol_name(AssemblerCtx* ctx, const char* in, char* out, size_t out_size) {
    if (!in || out_size == 0) return;

    if (in[0] == '.') {
        normalize_symbol_name(ctx, in, out, out_size);
        return;
    }

    const char* dot = 0;
    for (const char* p = in; *p; p++) {
        if (*p == '.') { dot = p; break; }
    }
    if (dot && dot[1] != 0) {
        size_t base_len = (size_t)(dot - in);
        size_t local_len = strlen(dot + 1);
        if (base_len + 1 + local_len >= out_size) {
            panic(ctx, "Symbol name too long");
        }
        memcpy(out, in, base_len);
        out[base_len] = '$';
        memcpy(out + base_len + 1, dot + 1, local_len + 1);
        return;
    }

    size_t len = strlen(in);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, in, len);
    out[len] = 0;
}

uint32_t resolve_abs_addr(AssemblerCtx* ctx, Symbol* s) {
    if (!s) panic(ctx, "Internal error: null symbol");

    if (s->bind == SYM_EXTERN || s->section == SEC_NULL) {
        panic(ctx, "External/undefined symbol in binary format");
    }

    if (s->section == SEC_ABS) return s->value;
    if (s->section == SEC_TEXT) return ctx->text_base + s->value;
    if (s->section == SEC_DATA) return ctx->data_base + s->value;
    if (s->section == SEC_BSS)  return ctx->bss_base  + s->value;

    return 0;
}

Symbol* sym_find(AssemblerCtx* ctx, const char* name) {
    if (!ctx->sym_hash || ctx->sym_hash_size <= 0) return 0;
    uint32_t h = sym_hash_calc(name);
    int mask = ctx->sym_hash_size - 1;

    for (int step = 0; step < ctx->sym_hash_size; step++) {
        int slot = (int)((h + (uint32_t)step) & (uint32_t)mask);
        int idx = ctx->sym_hash[slot];
        if (idx == -1) return 0;
        if (strcmp(ctx->symbols[idx].name, name) == 0) return &ctx->symbols[idx];
    }
    return 0;
}

Symbol* sym_add(AssemblerCtx* ctx, const char* name) {
    Symbol* s = sym_find(ctx, name);
    if (s) return s;
    if (!ctx->symbols || ctx->sym_capacity <= 0) {
        sym_table_init(ctx);
    }
    if (ctx->sym_count >= ctx->sym_capacity) {
        int new_cap = ctx->sym_capacity * 2;
        if (new_cap < 64) new_cap = 64;
        Symbol* ns = (Symbol*)malloc(sizeof(Symbol) * (size_t)new_cap);
        if (!ns) exit(1);
        if (ctx->symbols && ctx->sym_count > 0) {
            memcpy(ns, ctx->symbols, sizeof(Symbol) * (size_t)ctx->sym_count);
        }
        if (ctx->symbols) free(ctx->symbols);
        ctx->symbols = ns;
        ctx->sym_capacity = new_cap;
    }
    int idx = ctx->sym_count++;
    s = &ctx->symbols[idx];
    strcpy(s->name, name);
    s->bind = SYM_UNDEF;
    s->section = SEC_NULL;
    s->value = 0;
    s->elf_idx = 0;
    sym_hash_insert(ctx, idx);
    return s;
}

void sym_define_label(AssemblerCtx* ctx, const char* name) {
    Symbol* s = sym_find(ctx, name);

    if (ctx->pass == 1) {
        if (!s) s = sym_add(ctx, name);
        if (s->bind == SYM_UNDEF) s->bind = SYM_LOCAL;
        s->section = ctx->cur_sec;
    }

    if (s) {
        if (ctx->cur_sec == SEC_TEXT) s->value = ctx->text.size;
        else if (ctx->cur_sec == SEC_DATA) s->value = ctx->data.size;
        else if (ctx->cur_sec == SEC_BSS) s->value = ctx->bss.size;
    }
}

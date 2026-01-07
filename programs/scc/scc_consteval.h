// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_CONSTEVAL_H_INCLUDED
#define SCC_CONSTEVAL_H_INCLUDED

#include "scc_parser_base.h"
#include "scc_buffer.h"

typedef struct {
    Parser* p;
    SymTable* syms;
    Buffer* data;
    uint32_t str_id;
} SccConstEval;

static void scc_u32_to_dec(char out[16], uint32_t v) {
    char tmp[16];
    int n = 0;

    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }

    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static Symbol* scc_intern_string(SccConstEval* ce, const char* bytes, int len) {
    char namebuf[32];
    char dec[16];
    scc_u32_to_dec(dec, ce->str_id++);

    int n = 0;
    namebuf[n++] = '.';
    namebuf[n++] = 'L';
    namebuf[n++] = 's';
    namebuf[n++] = 't';
    namebuf[n++] = 'r';
    for (int i = 0; dec[i]; i++) namebuf[n++] = dec[i];
    namebuf[n] = 0;

    uint32_t off = ce->data->size;
    if (len) buf_write(ce->data, bytes, (uint32_t)len);
    buf_push_u8(ce->data, 0);

    return symtab_add_local_data(ce->syms, ce->p->arena, namebuf, off, (uint32_t)len + 1u);
}

static void scc_eval_const_u32(SccConstEval* ce, AstExpr* e, uint32_t* out_val, Symbol** out_reloc_sym) {
    *out_val = 0;
    *out_reloc_sym = 0;

    if (!e) return;

    if (e->kind == AST_EXPR_INT_LIT) {
        *out_val = (uint32_t)e->v.int_lit;
        return;
    }

    if (e->kind == AST_EXPR_STR) {
        Symbol* s = scc_intern_string(ce, e->v.str.bytes, e->v.str.len);
        *out_val = 0;
        *out_reloc_sym = s;
        return;
    }

    if (e->kind == AST_EXPR_CAST) {
        uint32_t v = 0;
        Symbol* rs = 0;
        scc_eval_const_u32(ce, e->v.cast.expr, &v, &rs);

        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_VOID) {
            *out_val = 0;
            *out_reloc_sym = 0;
            return;
        }

        if (e->v.cast.ty && (e->v.cast.ty->kind == TYPE_CHAR || e->v.cast.ty->kind == TYPE_UCHAR)) {
            if (rs) {
                scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Cannot cast relocatable address to char in global initializer");
            }
            v &= 0xFFu;
        }

        if (e->v.cast.ty && (e->v.cast.ty->kind == TYPE_SHORT || e->v.cast.ty->kind == TYPE_USHORT)) {
            if (rs) {
                scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Cannot cast relocatable address to short in global initializer");
            }
            v &= 0xFFFFu;
        }

        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_BOOL) {
            if (rs) {
                scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Cannot cast relocatable address to bool in global initializer");
            }
            v = (v != 0) ? 1u : 0u;
        }

        *out_val = v;
        *out_reloc_sym = rs;
        return;
    }

    if (e->kind == AST_EXPR_UNARY) {
        uint32_t v = 0;
        Symbol* rs = 0;
        scc_eval_const_u32(ce, e->v.unary.expr, &v, &rs);
        if (rs) {
            scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Relocatable address is not supported in unary global initializer");
        }

        int32_t sv = (int32_t)v;
        if (e->v.unary.op == AST_UNOP_NEG) sv = -sv;
        else if (e->v.unary.op == AST_UNOP_NOT) sv = (sv == 0) ? 1 : 0;
        *out_val = (uint32_t)sv;
        *out_reloc_sym = 0;
        return;
    }

    if (e->kind == AST_EXPR_BINARY) {
        uint32_t lv = 0;
        uint32_t rv = 0;
        Symbol* ls = 0;
        Symbol* rs = 0;
        scc_eval_const_u32(ce, e->v.binary.left, &lv, &ls);
        scc_eval_const_u32(ce, e->v.binary.right, &rv, &rs);
        if (ls || rs) {
            scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Relocatable address is not supported in binary global initializer");
        }

        if (e->v.binary.op == AST_BINOP_ADD) { *out_val = lv + rv; return; }
        if (e->v.binary.op == AST_BINOP_SUB) { *out_val = lv - rv; return; }
        if (e->v.binary.op == AST_BINOP_MUL) { *out_val = lv * rv; return; }
        if (e->v.binary.op == AST_BINOP_DIV) {
            if (rv == 0) scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Division by zero in global initializer");
            *out_val = (uint32_t)((int32_t)lv / (int32_t)rv);
            return;
        }
        if (e->v.binary.op == AST_BINOP_MOD) {
            if (rv == 0) scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Modulo by zero in global initializer");
            *out_val = (uint32_t)((int32_t)lv % (int32_t)rv);
            return;
        }

        scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Only arithmetic operators are supported in global initializers");
    }

    scc_fatal_at(ce->p->file, ce->p->src, e->tok.line, e->tok.col, "Non-constant global initializer");
}

#endif

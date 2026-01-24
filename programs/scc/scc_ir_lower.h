// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_IR_LOWER_H_INCLUDED
#define SCC_IR_LOWER_H_INCLUDED

#include "scc_ir.h"
#include "scc_parser_base.h"
#include "scc_buffer.h"

void ir_lower_unit_stub(IrModule* m, Parser* p, SymTable* syms, Buffer* data, uint32_t* io_str_id, AstUnit* u);

#ifdef SCC_IR_LOWER_IMPLEMENTATION

typedef struct {
    Parser* p;
    SymTable* syms;
    IrModule* m;
    Buffer* data;
    uint32_t str_id;
} IrLowerCtx;

static void ir_lower_u32_to_dec(char out[16], uint32_t v) {
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

static Symbol* ir_lower_intern_string(IrLowerCtx* lc, const char* bytes, int len) {
    if (!lc || !lc->p || !lc->syms || !lc->data) return 0;

    char namebuf[32];
    char dec[16];
    ir_lower_u32_to_dec(dec, lc->str_id++);

    int n = 0;
    namebuf[n++] = '.';
    namebuf[n++] = 'L';
    namebuf[n++] = 's';
    namebuf[n++] = 't';
    namebuf[n++] = 'r';
    for (int i = 0; dec[i]; i++) namebuf[n++] = dec[i];
    namebuf[n] = 0;

    uint32_t off = lc->data->size;
    if (len) buf_write(lc->data, bytes, (uint32_t)len);
    buf_push_u8(lc->data, 0);

    return symtab_add_local_data(lc->syms, lc->p->arena, namebuf, off, (uint32_t)len + 1u);
}

typedef struct IrLowerVarSlot IrLowerVarSlot;
struct IrLowerVarSlot {
    Var* var;
    IrValueId addr;
    IrLowerVarSlot* next;
};

typedef struct IrLowerLoop IrLowerLoop;
struct IrLowerLoop {
    IrBlockId break_target;
    IrBlockId continue_target;
    IrLowerLoop* next;
};

typedef struct {
    IrLowerCtx* lc;
    IrFunc* f;
    AstFunc* af;
    IrBlockId cur;
    IrLowerVarSlot* vars;
    IrLowerLoop* loops;
} IrLowerFuncCtx;

static IrValueId ir_lower_get_var_addr(IrLowerFuncCtx* fc, Var* v);
static Type* ir_lower_expr_type(IrLowerCtx* lc, AstExpr* e);
static IrValueId ir_lower_expr(IrLowerFuncCtx* fc, AstExpr* e);
static IrValueId ir_lower_cast_value(IrLowerFuncCtx* fc, IrValueId v, IrType* dst_ty, Token tok);
static IrValueId ir_lower_expr_bool(IrLowerFuncCtx* fc, AstExpr* e, Token tok);
static void ir_lower_stmt(IrLowerFuncCtx* fc, AstStmt* s);

static IrType* ir_type_from_scc(IrFunc* f, Type* t) {
    if (!t) return f->ty_i32;

    if (t->kind == TYPE_VOID) return f->ty_void;
    if (t->kind == TYPE_INT) return f->ty_i32;
    if (t->kind == TYPE_UINT) return f->ty_u32;
    if (t->kind == TYPE_LONG) return f->ty_i32;
    if (t->kind == TYPE_ULONG) return f->ty_u32;
    if (t->kind == TYPE_SHORT) return f->ty_i16;
    if (t->kind == TYPE_USHORT) return f->ty_u16;
    if (t->kind == TYPE_CHAR) return f->ty_i8;
    if (t->kind == TYPE_UCHAR) return f->ty_u8;
    if (t->kind == TYPE_BOOL) return f->ty_bool;

    if (t->kind == TYPE_PTR) {
        IrType* base = ir_type_from_scc(f, t->base);
        return ir_type_ptr(f, base);
    }

    return f->ty_i32;
}

static IrValueId ir_lower_addr(IrLowerFuncCtx* fc, AstExpr* e);

static IrValueId ir_lower_addr(IrLowerFuncCtx* fc, AstExpr* e) {
    if (!fc || !fc->lc || !fc->f || !e) return 0;

    if (e->kind == AST_EXPR_NAME) {
        if (e->v.name.var) {
            return ir_lower_get_var_addr(fc, e->v.name.var);
        }

        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(fc->lc->syms, e->v.name.name);
        if (!s || s->kind != SYM_DATA) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unknown identifier");
        }

        IrType* base = ir_type_from_scc(fc->f, s->ty);
        IrType* pty = ir_type_ptr(fc->f, base);
        return ir_emit_global_addr(fc->f, fc->cur, pty, s);
    }

    if (e->kind == AST_EXPR_UNARY && e->v.unary.op == AST_UNOP_DEREF) {
        Type* pt = ir_lower_expr_type(fc->lc, e->v.unary.expr);
        Type* base = (pt && pt->kind == TYPE_PTR) ? pt->base : 0;
        if (!base) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Cannot dereference non-pointer");
        }
        if (base->kind == TYPE_VOID) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Cannot dereference void*");
        }

        IrType* base_ir = ir_type_from_scc(fc->f, base);
        IrType* pty = ir_type_ptr(fc->f, base_ir);
        IrValueId pv = ir_lower_expr(fc, e->v.unary.expr);
        return ir_lower_cast_value(fc, pv, pty, e->tok);
    }

    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Expression is not addressable");
    return 0;
}

static int ir_block_is_terminated(IrFunc* f, IrBlockId b) {
    if (!f || b == 0 || b > f->block_count) return 1;
    return f->blocks[b - 1].term.kind != IR_TERM_INVALID;
}

static IrLowerVarSlot* ir_lower_find_var(IrLowerFuncCtx* fc, Var* v) {
    if (!fc || !v) return 0;
    for (IrLowerVarSlot* it = fc->vars; it; it = it->next) {
        if (it->var == v) return it;
    }
    return 0;
}

static IrValueId ir_lower_cast_value(IrLowerFuncCtx* fc, IrValueId v, IrType* dst_ty, Token tok) {
    if (!fc || !fc->f) return 0;
    if (!dst_ty) return v;

    if (dst_ty->kind == IR_TY_VOID) return 0;
    if (v == 0) return ir_emit_undef(fc->f, fc->cur, dst_ty);

    if (v > fc->f->value_count) {
        scc_fatal_at(fc->lc->p->file, fc->lc->p->src, tok.line, tok.col, "Internal error: invalid IR value id in cast");
    }

    IrType* src_ty = fc->f->values[v - 1].type;
    if (src_ty == dst_ty) return v;

    if (dst_ty->kind == IR_TY_BOOL) {
        if (src_ty && src_ty->kind == IR_TY_BOOL) return v;

        IrValueId vi32 = v;
        if (src_ty && src_ty->kind == IR_TY_PTR) vi32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
        else if (src_ty && (src_ty->kind == IR_TY_I16 || src_ty->kind == IR_TY_U16)) vi32 = ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);
        else if (src_ty && src_ty->kind == IR_TY_I8) vi32 = ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);
        else if (src_ty && src_ty->kind == IR_TY_BOOL) vi32 = ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);

        IrValueId z = ir_emit_iconst(fc->f, fc->cur, 0);
        return ir_emit_icmp(fc->f, fc->cur, IR_ICMP_NE, vi32, z);
    }

    if (dst_ty->kind == IR_TY_I32) {
        if (src_ty && src_ty->kind == IR_TY_PTR) return ir_emit_ptrtoint(fc->f, fc->cur, v);
        if (src_ty && src_ty->kind == IR_TY_I16) return ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
        if (src_ty && src_ty->kind == IR_TY_U16) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);
        if (src_ty && src_ty->kind == IR_TY_I8) return ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
        if (src_ty && (src_ty->kind == IR_TY_U8 || src_ty->kind == IR_TY_BOOL)) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);
        if (src_ty && src_ty->kind == IR_TY_U32) return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i32, v);
        return v;
    }

    if (dst_ty->kind == IR_TY_U32) {
        if (src_ty && src_ty->kind == IR_TY_PTR) {
            IrValueId i32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
            return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_u32, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_I32) return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_u32, v);
        if (src_ty && src_ty->kind == IR_TY_I16) {
            IrValueId i32 = ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_u32, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_U16) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_u32, v);
        if (src_ty && src_ty->kind == IR_TY_I8) {
            IrValueId i32 = ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_u32, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_U8) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_u32, v);
        if (src_ty && src_ty->kind == IR_TY_BOOL) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_u32, v);
        return v;
    }

    if (dst_ty->kind == IR_TY_I16) {
        if (src_ty && src_ty->kind == IR_TY_PTR) {
            IrValueId i32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i16, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_I32) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i16, v);
        if (src_ty && src_ty->kind == IR_TY_U32) {
            IrValueId i32 = ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i16, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_I8) return ir_emit_sext(fc->f, fc->cur, fc->f->ty_i16, v);
        if (src_ty && (src_ty->kind == IR_TY_U8 || src_ty->kind == IR_TY_BOOL)) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_i16, v);
        if (src_ty && src_ty->kind == IR_TY_U16) return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i16, v);
        return v;
    }

    if (dst_ty->kind == IR_TY_U16) {
        if (src_ty && src_ty->kind == IR_TY_PTR) {
            IrValueId i32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u16, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_I32) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u16, v);
        if (src_ty && src_ty->kind == IR_TY_U32) {
            IrValueId i32 = ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u16, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_I8) {
            IrValueId i32 = ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u16, i32);
        }
        if (src_ty && (src_ty->kind == IR_TY_U8 || src_ty->kind == IR_TY_BOOL)) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_u16, v);
        if (src_ty && src_ty->kind == IR_TY_I16) return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_u16, v);
        return v;
    }

    if (dst_ty->kind == IR_TY_I8) {
        if (src_ty && src_ty->kind == IR_TY_I32) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i8, v);
        if (src_ty && src_ty->kind == IR_TY_I16) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i8, v);
        if (src_ty && src_ty->kind == IR_TY_U16) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i8, v);
        if (src_ty && src_ty->kind == IR_TY_U32) {
            IrValueId i32 = ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i8, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_PTR) {
            IrValueId i32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_i8, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_BOOL) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_i8, v);
        return v;
    }

    if (dst_ty->kind == IR_TY_U8) {
        if (src_ty && src_ty->kind == IR_TY_I32) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u8, v);
        if (src_ty && src_ty->kind == IR_TY_U32) {
            IrValueId i32 = ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i32, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u8, i32);
        }
        if (src_ty && src_ty->kind == IR_TY_I16) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u8, v);
        if (src_ty && src_ty->kind == IR_TY_U16) return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u8, v);
        if (src_ty && src_ty->kind == IR_TY_I8) return ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_u8, v);
        if (src_ty && src_ty->kind == IR_TY_BOOL) return ir_emit_zext(fc->f, fc->cur, fc->f->ty_u8, v);
        if (src_ty && src_ty->kind == IR_TY_PTR) {
            IrValueId i32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
            return ir_emit_trunc(fc->f, fc->cur, fc->f->ty_u8, i32);
        }
        return v;
    }

    if (dst_ty->kind == IR_TY_PTR) {
        if (src_ty && src_ty->kind == IR_TY_PTR) {
            IrValueId i32 = ir_emit_ptrtoint(fc->f, fc->cur, v);
            return ir_emit_inttoptr(fc->f, fc->cur, dst_ty, i32);
        }

        IrValueId i32 = v;
        if (src_ty && src_ty->kind == IR_TY_I16) i32 = ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
        else if (src_ty && src_ty->kind == IR_TY_U16) i32 = ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);
        else if (src_ty && src_ty->kind == IR_TY_I8) i32 = ir_emit_sext(fc->f, fc->cur, fc->f->ty_i32, v);
        else if (src_ty && (src_ty->kind == IR_TY_U8 || src_ty->kind == IR_TY_BOOL)) i32 = ir_emit_zext(fc->f, fc->cur, fc->f->ty_i32, v);
        else if (src_ty && src_ty->kind == IR_TY_U32) i32 = ir_emit_bitcast(fc->f, fc->cur, fc->f->ty_i32, v);
        return ir_emit_inttoptr(fc->f, fc->cur, dst_ty, i32);
    }

    (void)tok;
    return v;
}

static uint32_t ir_lower_align_for_type(Type* t) {
    return type_align(t);
}

static IrValueId ir_lower_get_var_addr(IrLowerFuncCtx* fc, Var* v) {
    if (!fc || !fc->lc || !fc->f || !v) return 0;

    IrLowerVarSlot* slot = ir_lower_find_var(fc, v);
    if (slot) return slot->addr;

    IrType* ty = ir_type_from_scc(fc->f, v->ty);
    uint32_t al = ir_lower_align_for_type(v->ty);
    IrValueId addr = ir_emit_alloca(fc->f, fc->f->entry, ty, al);

    IrLowerVarSlot* ns = (IrLowerVarSlot*)arena_alloc(fc->f->arena, sizeof(IrLowerVarSlot), 8);
    memset(ns, 0, sizeof(*ns));
    ns->var = v;
    ns->addr = addr;
    ns->next = fc->vars;
    fc->vars = ns;

    if (v->kind == VAR_PARAM) {
        int idx = (v->ebp_offset - 8) / 4;
        if (idx < 0 || idx >= (int)fc->f->param_count) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, fc->lc->p->tok.line, fc->lc->p->tok.col, "Internal error: invalid parameter index");
        }
        IrValueId pv = fc->f->blocks[fc->f->entry - 1].params[idx];
        ir_emit_store(fc->f, fc->f->entry, addr, pv);
    }

    return addr;
}

static Type* ir_lower_lvalue_type(IrLowerCtx* lc, AstExpr* e) {
    if (!lc || !e) return 0;

    if (e->kind == AST_EXPR_NAME) {
        if (e->v.name.var) return e->v.name.var->ty;
        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(lc->syms, e->v.name.name);
        return s ? s->ty : 0;
    }

    if (e->kind == AST_EXPR_UNARY && e->v.unary.op == AST_UNOP_DEREF) {
        Type* pt = ir_lower_expr_type(lc, e->v.unary.expr);
        if (pt && pt->kind == TYPE_PTR) return pt->base;
        return 0;
    }

    return 0;
}

static TypeKind ir_tc_uac_common_int_kind(TypeKind ak, TypeKind bk);
static Type* ir_tc_uac_type_from_kind(IrLowerCtx* lc, TypeKind k);
static Type* ir_tc_uac_common_int_type(IrLowerCtx* lc, Type* a, Type* b);
static Type* ir_tc_uac_promote_int_type(IrLowerCtx* lc, Type* a);

static Type* ir_lower_expr_type(IrLowerCtx* lc, AstExpr* e) {
    if (!lc || !e) return 0;

    if (e->kind == AST_EXPR_INT_LIT) return 0;

    if (e->kind == AST_EXPR_STR) {
        Type* ch = type_char(lc->p);
        return type_ptr_to(lc->p, ch);
    }

    if (e->kind == AST_EXPR_CAST) return e->v.cast.ty;

    if (e->kind == AST_EXPR_NAME) {
        if (e->v.name.var) return e->v.name.var->ty;
        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(lc->syms, e->v.name.name);
        return s ? s->ty : 0;
    }

    if (e->kind == AST_EXPR_CALL) {
        Symbol* s = symtab_find(lc->syms, e->v.call.callee);
        if (s) return s->ftype.ret;
        return 0;
    }

    if (e->kind == AST_EXPR_UNARY) {
        if (e->v.unary.op == AST_UNOP_ADDR) {
            Type* base = ir_lower_lvalue_type(lc, e->v.unary.expr);
            return base ? type_ptr_to(lc->p, base) : 0;
        }
        if (e->v.unary.op == AST_UNOP_DEREF) {
            Type* pt = ir_lower_expr_type(lc, e->v.unary.expr);
            if (pt && pt->kind == TYPE_PTR) return pt->base;
            return 0;
        }
        if (e->v.unary.op == AST_UNOP_POS || e->v.unary.op == AST_UNOP_NEG) {
            Type* ot = ir_lower_expr_type(lc, e->v.unary.expr);
            return ir_tc_uac_promote_int_type(lc, ot);
        }
        if (e->v.unary.op == AST_UNOP_NOT) {
            return type_bool(lc->p);
        }
        if (e->v.unary.op == AST_UNOP_BNOT) {
            Type* ot = ir_lower_expr_type(lc, e->v.unary.expr);
            return ir_tc_uac_promote_int_type(lc, ot);
        }
        return 0;
    }

    if (e->kind == AST_EXPR_ASSIGN) {
        return ir_lower_lvalue_type(lc, e->v.assign.left);
    }

    if (e->kind == AST_EXPR_BINARY) {
        Type* lt = ir_lower_expr_type(lc, e->v.binary.left);
        Type* rt = ir_lower_expr_type(lc, e->v.binary.right);

        if (e->v.binary.op == AST_BINOP_ANDAND || e->v.binary.op == AST_BINOP_OROR) {
            return type_bool(lc->p);
        }

        if (e->v.binary.op == AST_BINOP_EQ || e->v.binary.op == AST_BINOP_NE || e->v.binary.op == AST_BINOP_LT || e->v.binary.op == AST_BINOP_LE || e->v.binary.op == AST_BINOP_GT || e->v.binary.op == AST_BINOP_GE) {
            return type_bool(lc->p);
        }

        if (e->v.binary.op == AST_BINOP_ADD) {
            if (lt && lt->kind == TYPE_PTR && (!rt || rt->kind != TYPE_PTR)) return lt;
            if (rt && rt->kind == TYPE_PTR && (!lt || lt->kind != TYPE_PTR)) return rt;
            return ir_tc_uac_common_int_type(lc, lt, rt);
        }

        if (e->v.binary.op == AST_BINOP_SUB) {
            if (lt && lt->kind == TYPE_PTR) {
                if (rt && rt->kind == TYPE_PTR) return 0;
                return lt;
            }
            return ir_tc_uac_common_int_type(lc, lt, rt);
        }

        if (e->v.binary.op == AST_BINOP_MUL || e->v.binary.op == AST_BINOP_DIV || e->v.binary.op == AST_BINOP_MOD) {
            return ir_tc_uac_common_int_type(lc, lt, rt);
        }

        if (e->v.binary.op == AST_BINOP_BAND || e->v.binary.op == AST_BINOP_BXOR || e->v.binary.op == AST_BINOP_BOR) {
            return ir_tc_uac_common_int_type(lc, lt, rt);
        }

        if (e->v.binary.op == AST_BINOP_SHL || e->v.binary.op == AST_BINOP_SHR) {
            return ir_tc_uac_promote_int_type(lc, lt);
        }

        return 0;
    }

    return 0;
}

static int ir_tc_is_int_type(Type* t) {
    if (!t) return 1;
    return type_is_integer(t);
}

static int ir_tc_is_scalar_type(Type* t) {
    if (!t) return 1;
    return type_is_scalar(t);
}

static int ir_tc_uac_is_unsigned_kind(TypeKind k) {
    return k == TYPE_UINT || k == TYPE_ULONG || k == TYPE_USHORT || k == TYPE_UCHAR;
}

static TypeKind ir_tc_uac_promote_kind(TypeKind k) {
    if (k == TYPE_BOOL || k == TYPE_CHAR || k == TYPE_UCHAR || k == TYPE_SHORT || k == TYPE_USHORT) {
        return TYPE_INT;
    }
    return k;
}

static uint32_t ir_tc_uac_size_kind(TypeKind k) {
    if (k == TYPE_BOOL) return 1;
    if (k == TYPE_CHAR) return 1;
    if (k == TYPE_UCHAR) return 1;
    if (k == TYPE_SHORT) return 2;
    if (k == TYPE_USHORT) return 2;
    if (k == TYPE_INT) return 4;
    if (k == TYPE_UINT) return 4;
    if (k == TYPE_LONG) return 4;
    if (k == TYPE_ULONG) return 4;
    return 0;
}

static int ir_tc_uac_rank_kind(TypeKind k) {
    if (k == TYPE_UINT) k = TYPE_INT;
    if (k == TYPE_ULONG) k = TYPE_LONG;
    if (k == TYPE_USHORT) k = TYPE_SHORT;
    if (k == TYPE_UCHAR) k = TYPE_CHAR;

    if (k == TYPE_BOOL) return 1;
    if (k == TYPE_CHAR) return 2;
    if (k == TYPE_SHORT) return 3;
    if (k == TYPE_INT) return 4;
    if (k == TYPE_LONG) return 5;
    return 0;
}

static TypeKind ir_tc_uac_unsigned_of_signed_kind(TypeKind k) {
    if (k == TYPE_INT) return TYPE_UINT;
    if (k == TYPE_LONG) return TYPE_ULONG;
    if (k == TYPE_SHORT) return TYPE_USHORT;
    if (k == TYPE_CHAR) return TYPE_UCHAR;
    return k;
}

static TypeKind ir_tc_uac_common_int_kind(TypeKind ak, TypeKind bk) {
    if (ak == 0) ak = TYPE_INT;
    if (bk == 0) bk = TYPE_INT;

    ak = ir_tc_uac_promote_kind(ak);
    bk = ir_tc_uac_promote_kind(bk);

    if (ak == bk) return ak;

    int au = ir_tc_uac_is_unsigned_kind(ak);
    int bu = ir_tc_uac_is_unsigned_kind(bk);
    int ar = ir_tc_uac_rank_kind(ak);
    int br = ir_tc_uac_rank_kind(bk);

    if (au == bu) return (ar >= br) ? ak : bk;

    TypeKind uk = au ? ak : bk;
    TypeKind sk = au ? bk : ak;
    int ur = au ? ar : br;
    int sr = au ? br : ar;

    if (ur >= sr) return uk;

    uint32_t usz = ir_tc_uac_size_kind(uk);
    uint32_t ssz = ir_tc_uac_size_kind(sk);
    if (ssz > usz) return sk;
    return ir_tc_uac_unsigned_of_signed_kind(sk);
}

static Type* ir_tc_uac_type_from_kind(IrLowerCtx* lc, TypeKind k) {
    if (!lc || !lc->p) return 0;
    if (k == TYPE_INT) return type_int(lc->p);
    if (k == TYPE_UINT) return type_uint(lc->p);
    if (k == TYPE_LONG) return type_long(lc->p);
    if (k == TYPE_ULONG) return type_ulong(lc->p);
    if (k == TYPE_SHORT) return type_short(lc->p);
    if (k == TYPE_USHORT) return type_ushort(lc->p);
    if (k == TYPE_CHAR) return type_char(lc->p);
    if (k == TYPE_UCHAR) return type_uchar(lc->p);
    if (k == TYPE_BOOL) return type_bool(lc->p);
    return 0;
}

static Type* ir_tc_uac_common_int_type(IrLowerCtx* lc, Type* a, Type* b) {
    TypeKind ak = a ? a->kind : TYPE_INT;
    TypeKind bk = b ? b->kind : TYPE_INT;
    if (!type_is_integer(a) && a) return 0;
    if (!type_is_integer(b) && b) return 0;
    TypeKind ck = ir_tc_uac_common_int_kind(ak, bk);
    return ir_tc_uac_type_from_kind(lc, ck);
}

static Type* ir_tc_uac_promote_int_type(IrLowerCtx* lc, Type* a) {
    TypeKind ak = a ? a->kind : TYPE_INT;
    if (!type_is_integer(a) && a) return 0;
    TypeKind pk = ir_tc_uac_promote_kind(ak);
    return ir_tc_uac_type_from_kind(lc, pk);
}

static int ir_tc_is_null_ptr_const(AstExpr* e) {
    return e && e->kind == AST_EXPR_INT_LIT && e->v.int_lit == 0;
}

static int ir_tc_ptr_qual_ok(Type* dst, Type* src) {
    if (!dst || !src) return 0;
    if (src->is_const && !dst->is_const) return 0;
    if (dst->kind == TYPE_PTR && src->kind == TYPE_PTR) {
        return ir_tc_ptr_qual_ok(dst->base, src->base);
    }
    return 1;
}

static void ir_tc_check_assign(IrLowerCtx* lc, Token tok, Type* dst, AstExpr* src_expr) {
    if (!lc || !lc->p) return;

    Type* src = ir_lower_expr_type(lc, src_expr);
    int src_is_null = ir_tc_is_null_ptr_const(src_expr);

    if (dst && dst->kind == TYPE_VOID) {
        scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Cannot convert to void");
    }
    if (src && src->kind == TYPE_VOID) {
        scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Void value is not allowed here");
    }

    if (dst && dst->kind == TYPE_PTR) {
        if (src_is_null) return;
        if (!src || src->kind != TYPE_PTR) {
            scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Incompatible types in pointer conversion");
        }

        Type* db = dst->base;
        Type* sb = src->base;
        if (!db || !sb) {
            scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Internal error: invalid pointer type");
        }

        if (db->kind == TYPE_VOID || sb->kind == TYPE_VOID) {
            if (!ir_tc_ptr_qual_ok(db, sb)) {
                scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Discards const qualifier in pointer conversion");
            }
            return;
        }

        if (!type_compatible_unqualified(db, sb)) {
            scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Incompatible pointer types");
        }
        if (!ir_tc_ptr_qual_ok(db, sb)) {
            scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Discards const qualifier in pointer conversion");
        }
        return;
    }

    if (ir_tc_is_int_type(dst)) {
        if (!ir_tc_is_int_type(src)) {
            scc_fatal_at(lc->p->file, lc->p->src, tok.line, tok.col, "Cannot implicitly convert pointer to integer");
        }
        return;
    }
}

static int ir_lower_is_unsigned_int_type(Type* t) {
    if (!t) return 0;
    if (t->kind == TYPE_UINT) return 1;
    if (t->kind == TYPE_ULONG) return 1;
    if (t->kind == TYPE_USHORT) return 1;
    if (t->kind == TYPE_UCHAR) return 1;
    return 0;
}

static int ir_lower_expr_is_unsigned(IrLowerCtx* lc, AstExpr* e) {
    if (!lc || !e) return 0;

    if (e->kind == AST_EXPR_CAST) return ir_lower_is_unsigned_int_type(e->v.cast.ty);

    if (e->kind == AST_EXPR_NAME) {
        if (e->v.name.var) return ir_lower_is_unsigned_int_type(e->v.name.var->ty);
        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(lc->syms, e->v.name.name);
        return s ? ir_lower_is_unsigned_int_type(s->ty) : 0;
    }

    if (e->kind == AST_EXPR_CALL) {
        Symbol* s = symtab_find(lc->syms, e->v.call.callee);
        if (!s) return 0;
        return ir_lower_is_unsigned_int_type(s->ftype.ret);
    }

    if (e->kind == AST_EXPR_UNARY) {
        return ir_lower_expr_is_unsigned(lc, e->v.unary.expr);
    }

    if (e->kind == AST_EXPR_ASSIGN) {
        Type* t = ir_lower_lvalue_type(lc, e->v.assign.left);
        return ir_lower_is_unsigned_int_type(t);
    }

    if (e->kind == AST_EXPR_BINARY) {
        return ir_lower_expr_is_unsigned(lc, e->v.binary.left) || ir_lower_expr_is_unsigned(lc, e->v.binary.right);
    }

    return 0;
}

static IrValueId ir_lower_expr(IrLowerFuncCtx* fc, AstExpr* e);

static IrValueId ir_lower_expr(IrLowerFuncCtx* fc, AstExpr* e) {
    if (!fc || !fc->lc || !fc->f) return 0;
    if (!e) return ir_emit_iconst(fc->f, fc->cur, 0);

    if (e->kind == AST_EXPR_INT_LIT) {
        return ir_emit_iconst(fc->f, fc->cur, e->v.int_lit);
    }

    if (e->kind == AST_EXPR_NAME) {
        if (e->v.name.var) {
            IrValueId addr = ir_lower_get_var_addr(fc, e->v.name.var);
            IrType* ty = ir_type_from_scc(fc->f, e->v.name.var->ty);
            return ir_emit_load(fc->f, fc->cur, ty, addr);
        }

        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(fc->lc->syms, e->v.name.name);
        if (!s || s->kind != SYM_DATA) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unknown identifier");
        }

        IrType* ty = ir_type_from_scc(fc->f, s->ty);
        IrType* pty = ir_type_ptr(fc->f, ty);
        IrValueId addr = ir_emit_global_addr(fc->f, fc->cur, pty, s);
        return ir_emit_load(fc->f, fc->cur, ty, addr);
    }

    if (e->kind == AST_EXPR_STR) {
        if (!fc->lc->data) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: string literal lowering requires data buffer");
        }
        Symbol* s = ir_lower_intern_string(fc->lc, e->v.str.bytes, e->v.str.len);
        IrType* pty = ir_type_ptr(fc->f, fc->f->ty_i8);
        return ir_emit_global_addr(fc->f, fc->cur, pty, s);
    }

    if (e->kind == AST_EXPR_CAST) {
        IrValueId v = ir_lower_expr(fc, e->v.cast.expr);
        IrType* dst = ir_type_from_scc(fc->f, e->v.cast.ty);
        return ir_lower_cast_value(fc, v, dst, e->tok);
    }

    if (e->kind == AST_EXPR_CALL) {
        if (strcmp(e->v.call.callee, "__syscall") == 0) {
            if (e->v.call.arg_count != 4) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "__syscall requires exactly 4 arguments");
            }
            IrValueId n = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.call.args[0]), fc->f->ty_i32, e->tok);
            IrValueId a1 = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.call.args[1]), fc->f->ty_i32, e->tok);
            IrValueId a2 = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.call.args[2]), fc->f->ty_i32, e->tok);
            IrValueId a3 = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.call.args[3]), fc->f->ty_i32, e->tok);
            return ir_emit_syscall(fc->f, fc->cur, n, a1, a2, a3);
        }

        Symbol* s = symtab_find(fc->lc->syms, e->v.call.callee);

        if (!s || s->kind != SYM_FUNC) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Call to undeclared function");
        }

        if (s->ftype.param_count != e->v.call.arg_count) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Argument count mismatch in call");
        }

        uint32_t argc = (uint32_t)e->v.call.arg_count;
        IrValueId* args = 0;
        if (argc) args = (IrValueId*)arena_alloc(fc->f->arena, argc * sizeof(IrValueId), 8);

        for (int i = e->v.call.arg_count - 1; i >= 0; i--) {
            AstExpr* ae = e->v.call.args[i];
            Type* pt = s->ftype.params ? s->ftype.params[i] : 0;
            ir_tc_check_assign(fc->lc, ae ? ae->tok : e->tok, pt, ae);

            IrValueId av = ir_lower_expr(fc, ae);
            IrType* aty = ir_type_from_scc(fc->f, pt);
            args[i] = ir_lower_cast_value(fc, av, aty, ae ? ae->tok : e->tok);
        }

        IrType* ret_ty = ir_type_from_scc(fc->f, s->ftype.ret);
        return ir_emit_call(fc->f, fc->cur, ret_ty, s, args, argc);
    }

    if (e->kind == AST_EXPR_UNARY) {
        if (e->v.unary.op == AST_UNOP_ADDR) {
            return ir_lower_addr(fc, e->v.unary.expr);
        }
        if (e->v.unary.op == AST_UNOP_DEREF) {
            Type* pt = ir_lower_expr_type(fc->lc, e->v.unary.expr);
            Type* base = (pt && pt->kind == TYPE_PTR) ? pt->base : 0;
            if (!base) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Cannot dereference non-pointer");
            }
            if (base->kind == TYPE_VOID) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Cannot dereference void*");
            }

            IrType* base_ir = ir_type_from_scc(fc->f, base);
            IrType* pty = ir_type_ptr(fc->f, base_ir);

            IrValueId pv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.unary.expr), pty, e->tok);
            return ir_emit_load(fc->f, fc->cur, base_ir, pv);
        }

        Type* ut = ir_lower_expr_type(fc->lc, e->v.unary.expr);
        if (ut && ut->kind == TYPE_VOID) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Void value is not allowed here");
        }
        if ((e->v.unary.op == AST_UNOP_POS || e->v.unary.op == AST_UNOP_NEG) && !ir_tc_is_int_type(ut)) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unary + or - requires integer operand");
        }
        if (e->v.unary.op == AST_UNOP_BNOT && !ir_tc_is_int_type(ut)) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unary ~ requires integer operand");
        }
        if (e->v.unary.op == AST_UNOP_NOT && !ir_tc_is_scalar_type(ut)) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unary ! requires scalar operand");
        }

        if (e->v.unary.op == AST_UNOP_NOT) {
            IrValueId v = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.unary.expr), fc->f->ty_i32, e->tok);
            IrValueId z = ir_emit_iconst(fc->f, fc->cur, 0);
            return ir_emit_icmp(fc->f, fc->cur, IR_ICMP_EQ, v, z);
        }

        IrType* ity = fc->f->ty_i32;
        if (e->v.unary.op == AST_UNOP_POS || e->v.unary.op == AST_UNOP_NEG || e->v.unary.op == AST_UNOP_BNOT) {
            Type* pt = ir_tc_uac_promote_int_type(fc->lc, ut);
            if (pt && ir_tc_uac_is_unsigned_kind(pt->kind)) ity = fc->f->ty_u32;
        }

        IrValueId v = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.unary.expr), ity, e->tok);

        if (e->v.unary.op == AST_UNOP_POS) return v;
        if (e->v.unary.op == AST_UNOP_NEG) {
            IrValueId z = ir_lower_cast_value(fc, ir_emit_iconst(fc->f, fc->cur, 0), ity, e->tok);
            return ir_emit_bin(fc->f, fc->cur, IR_INSTR_SUB, ity, z, v);
        }
        if (e->v.unary.op == AST_UNOP_BNOT) {
            IrValueId ones = 0;
            if (ity == fc->f->ty_u32) {
                ones = ir_emit_uconst(fc->f, fc->cur, 0xFFFFFFFFu);
            } else {
                ones = ir_emit_iconst(fc->f, fc->cur, -1);
                ones = ir_lower_cast_value(fc, ones, ity, e->tok);
            }
            return ir_emit_bin(fc->f, fc->cur, IR_INSTR_XOR, ity, v, ones);
        }
    }

    if (e->kind == AST_EXPR_ASSIGN) {
        Type* lvt = ir_lower_lvalue_type(fc->lc, e->v.assign.left);
        if (!lvt) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Invalid assignment target");
        }

        if (lvt->is_const) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Assignment to const lvalue");
        }

        IrType* lvir = ir_type_from_scc(fc->f, lvt);
        if (lvir && lvir->kind == IR_TY_VOID) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Cannot assign to void lvalue");
        }

        IrValueId addr = ir_lower_addr(fc, e->v.assign.left);

        if (e->v.assign.op == (AstBinOp)0) {
            ir_tc_check_assign(fc->lc, e->tok, lvt, e->v.assign.right);

            IrValueId rv = ir_lower_expr(fc, e->v.assign.right);
            IrValueId cv = ir_lower_cast_value(fc, rv, lvir, e->tok);
            ir_emit_store(fc->f, fc->cur, addr, cv);
            return cv;
        }

        AstBinOp op = e->v.assign.op;

        if (op == AST_BINOP_ADD || op == AST_BINOP_SUB) {
            Type* rt = ir_lower_expr_type(fc->lc, e->v.assign.right);
            int lptr = (lvt && lvt->kind == TYPE_PTR);
            int rptr = (rt && rt->kind == TYPE_PTR);

            if (lptr) {
                if (rptr) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unsupported pointer arithmetic in compound assignment");
                }

                if (!ir_tc_is_int_type(rt)) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Pointer offset must be integer");
                }

                uint32_t scale = type_size(lvt->base);
                if (scale == 0) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Pointer arithmetic on void* is not supported");
                }

                IrType* base_ir = ir_type_from_scc(fc->f, lvt->base);
                IrType* ptr_ir = ir_type_ptr(fc->f, base_ir);

                IrValueId old_lv = ir_emit_load(fc->f, fc->cur, lvir, addr);
                IrValueId basev = ir_lower_cast_value(fc, old_lv, ptr_ir, e->tok);

                IrValueId offv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.assign.right), fc->f->ty_i32, e->tok);
                if (scale != 1) {
                    IrValueId sc = ir_emit_iconst(fc->f, fc->cur, (int32_t)scale);
                    offv = ir_emit_bin(fc->f, fc->cur, IR_INSTR_MUL, fc->f->ty_i32, offv, sc);
                }
                if (op == AST_BINOP_SUB) {
                    IrValueId z = ir_emit_iconst(fc->f, fc->cur, 0);
                    offv = ir_emit_bin(fc->f, fc->cur, IR_INSTR_SUB, fc->f->ty_i32, z, offv);
                }

                IrValueId res = ir_emit_ptr_add(fc->f, fc->cur, ptr_ir, basev, offv);
                IrValueId cv = ir_lower_cast_value(fc, res, lvir, e->tok);
                ir_emit_store(fc->f, fc->cur, addr, cv);
                return cv;
            }
        }

        Type* rt = ir_lower_expr_type(fc->lc, e->v.assign.right);
        if (!ir_tc_is_int_type(lvt) || !ir_tc_is_int_type(rt)) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Compound assignment requires integer operands");
        }

        IrInstrKind k = IR_INSTR_INVALID;
        IrType* ity = fc->f->ty_i32;
        int is_unsigned = 0;

        if (op == AST_BINOP_SHL || op == AST_BINOP_SHR) {
            Type* pt = ir_tc_uac_promote_int_type(fc->lc, lvt);
            if (!pt) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid shift type");
            }
            is_unsigned = ir_tc_uac_is_unsigned_kind(pt->kind);
            ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;

            if (op == AST_BINOP_SHL) k = IR_INSTR_SHL;
            else k = is_unsigned ? IR_INSTR_SHR : IR_INSTR_SAR;
        } else {
            Type* ct = ir_tc_uac_common_int_type(fc->lc, lvt, rt);
            if (!ct) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid arithmetic type");
            }
            is_unsigned = ir_tc_uac_is_unsigned_kind(ct->kind);
            ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;

            if (op == AST_BINOP_ADD) k = IR_INSTR_ADD;
            else if (op == AST_BINOP_SUB) k = IR_INSTR_SUB;
            else if (op == AST_BINOP_MUL) k = IR_INSTR_MUL;
            else if (op == AST_BINOP_DIV) k = is_unsigned ? IR_INSTR_UDIV : IR_INSTR_SDIV;
            else if (op == AST_BINOP_MOD) k = is_unsigned ? IR_INSTR_UREM : IR_INSTR_SREM;
            else if (op == AST_BINOP_BAND) k = IR_INSTR_AND;
            else if (op == AST_BINOP_BOR) k = IR_INSTR_OR;
            else if (op == AST_BINOP_BXOR) k = IR_INSTR_XOR;
            else {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Compound assignment operator not supported");
            }
        }

        IrValueId old_lv = ir_lower_cast_value(fc, ir_emit_load(fc->f, fc->cur, lvir, addr), ity, e->tok);
        IrValueId rv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.assign.right), ity, e->tok);

        IrValueId res = ir_emit_bin(fc->f, fc->cur, k, ity, old_lv, rv);
        IrValueId cv = ir_lower_cast_value(fc, res, lvir, e->tok);
        ir_emit_store(fc->f, fc->cur, addr, cv);
        return cv;
    }

    if (e->kind == AST_EXPR_BINARY) {
        if (e->v.binary.op == AST_BINOP_ANDAND || e->v.binary.op == AST_BINOP_OROR) {
            IrValueId lv = ir_lower_expr_bool(fc, e->v.binary.left, e->tok);
            IrBlockId rhs_b = ir_block_new(fc->f);
            IrBlockId join_b = ir_block_new(fc->f);
            IrValueId res = ir_block_add_param(fc->f, join_b, fc->f->ty_bool);

            if (e->v.binary.op == AST_BINOP_ANDAND) {
                IrValueId fargs[1] = { lv };
                ir_set_term_condbr(fc->f, fc->cur, lv, rhs_b, 0, 0, join_b, fargs, 1);

                fc->cur = rhs_b;
                IrValueId rv = ir_lower_expr_bool(fc, e->v.binary.right, e->tok);
                IrValueId targs[1] = { rv };
                if (!ir_block_is_terminated(fc->f, fc->cur)) {
                    ir_set_term_br(fc->f, fc->cur, join_b, targs, 1);
                }

                fc->cur = join_b;
                return res;
            }

            if (e->v.binary.op == AST_BINOP_OROR) {
                IrValueId targs[1] = { lv };
                ir_set_term_condbr(fc->f, fc->cur, lv, join_b, targs, 1, rhs_b, 0, 0);

                fc->cur = rhs_b;
                IrValueId rv = ir_lower_expr_bool(fc, e->v.binary.right, e->tok);
                IrValueId fargs[1] = { rv };
                if (!ir_block_is_terminated(fc->f, fc->cur)) {
                    ir_set_term_br(fc->f, fc->cur, join_b, fargs, 1);
                }

                fc->cur = join_b;
                return res;
            }
        }

        Type* lt = ir_lower_expr_type(fc->lc, e->v.binary.left);
        Type* rt = ir_lower_expr_type(fc->lc, e->v.binary.right);
        int lptr = (lt && lt->kind == TYPE_PTR);
        int rptr = (rt && rt->kind == TYPE_PTR);

        if (e->v.binary.op == AST_BINOP_ADD || e->v.binary.op == AST_BINOP_SUB) {
            if ((e->v.binary.op == AST_BINOP_ADD && (lptr ^ rptr)) || (e->v.binary.op == AST_BINOP_SUB && lptr && !rptr)) {
                Type* pty = lptr ? lt : rt;
                uint32_t scale = type_size(pty->base);
                if (scale == 0) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Pointer arithmetic on void* is not supported");
                }

                AstExpr* base_e = lptr ? e->v.binary.left : e->v.binary.right;
                AstExpr* off_e = lptr ? e->v.binary.right : e->v.binary.left;

                Type* off_t = ir_lower_expr_type(fc->lc, off_e);
                if (off_t && off_t->kind == TYPE_VOID) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Void value is not allowed here");
                }
                if (!ir_tc_is_int_type(off_t)) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Pointer offset must be integer");
                }

                IrType* base_ir = ir_type_from_scc(fc->f, pty->base);
                IrType* ptr_ir = ir_type_ptr(fc->f, base_ir);

                IrValueId basev = ir_lower_cast_value(fc, ir_lower_expr(fc, base_e), ptr_ir, e->tok);
                IrValueId offv = ir_lower_cast_value(fc, ir_lower_expr(fc, off_e), fc->f->ty_i32, e->tok);

                if (scale != 1) {
                    IrValueId sc = ir_emit_iconst(fc->f, fc->cur, (int32_t)scale);
                    offv = ir_emit_bin(fc->f, fc->cur, IR_INSTR_MUL, fc->f->ty_i32, offv, sc);
                }

                if (e->v.binary.op == AST_BINOP_SUB) {
                    IrValueId z = ir_emit_iconst(fc->f, fc->cur, 0);
                    offv = ir_emit_bin(fc->f, fc->cur, IR_INSTR_SUB, fc->f->ty_i32, z, offv);
                }

                return ir_emit_ptr_add(fc->f, fc->cur, ptr_ir, basev, offv);
            }

            if (e->v.binary.op == AST_BINOP_ADD && (lptr && rptr)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unsupported pointer addition");
            }

            if (e->v.binary.op == AST_BINOP_SUB && rptr && !lptr) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Cannot subtract a pointer from an integer");
            }

            if (e->v.binary.op == AST_BINOP_SUB && lptr && rptr) {
                if (!type_compatible_unqualified(lt->base, rt->base)) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Pointer subtraction requires compatible pointer types");
                }

                uint32_t scale = type_size(lt->base);
                if (scale == 0) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Pointer arithmetic on void* is not supported");
                }

                IrValueId li = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.left), fc->f->ty_i32, e->tok);
                IrValueId ri = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.right), fc->f->ty_i32, e->tok);
                IrValueId diff = ir_emit_bin(fc->f, fc->cur, IR_INSTR_SUB, fc->f->ty_i32, li, ri);

                if (scale == 1) return diff;
                if (scale == 2) {
                    IrValueId sc = ir_emit_iconst(fc->f, fc->cur, 2);
                    return ir_emit_bin(fc->f, fc->cur, IR_INSTR_SDIV, fc->f->ty_i32, diff, sc);
                }
                if (scale == 4) {
                    IrValueId sc = ir_emit_iconst(fc->f, fc->cur, 4);
                    return ir_emit_bin(fc->f, fc->cur, IR_INSTR_SDIV, fc->f->ty_i32, diff, sc);
                }

                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Unsupported pointer difference scale");
            }
        }

        if (e->v.binary.op == AST_BINOP_MUL || e->v.binary.op == AST_BINOP_DIV || e->v.binary.op == AST_BINOP_MOD) {
            if (!ir_tc_is_int_type(lt) || !ir_tc_is_int_type(rt)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Arithmetic operator requires integer operands");
            }
            Type* ct = ir_tc_uac_common_int_type(fc->lc, lt, rt);
            if (!ct) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid arithmetic type");
            }
            int is_unsigned = ir_tc_uac_is_unsigned_kind(ct->kind);
            IrType* ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;

            IrValueId lv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.left), ity, e->tok);
            IrValueId rv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.right), ity, e->tok);

            IrInstrKind k = IR_INSTR_INVALID;
            if (e->v.binary.op == AST_BINOP_MUL) k = IR_INSTR_MUL;
            else if (e->v.binary.op == AST_BINOP_DIV) k = is_unsigned ? IR_INSTR_UDIV : IR_INSTR_SDIV;
            else if (e->v.binary.op == AST_BINOP_MOD) k = is_unsigned ? IR_INSTR_UREM : IR_INSTR_SREM;
            return ir_emit_bin(fc->f, fc->cur, k, ity, lv, rv);
        }

        if (e->v.binary.op == AST_BINOP_EQ || e->v.binary.op == AST_BINOP_NE || e->v.binary.op == AST_BINOP_LT || e->v.binary.op == AST_BINOP_LE || e->v.binary.op == AST_BINOP_GT || e->v.binary.op == AST_BINOP_GE) {
            Type* clt = ir_lower_expr_type(fc->lc, e->v.binary.left);
            Type* crt = ir_lower_expr_type(fc->lc, e->v.binary.right);
            int is_ptr = (clt && clt->kind == TYPE_PTR) || (crt && crt->kind == TYPE_PTR);
            int l_is_ptr = (clt && clt->kind == TYPE_PTR);
            int r_is_ptr = (crt && crt->kind == TYPE_PTR);

            if ((clt && clt->kind == TYPE_VOID) || (crt && crt->kind == TYPE_VOID)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Void value is not allowed here");
            }

            if (is_ptr) {
                int l_null = ir_tc_is_null_ptr_const(e->v.binary.left);
                int r_null = ir_tc_is_null_ptr_const(e->v.binary.right);

                int is_rel = !(e->v.binary.op == AST_BINOP_EQ || e->v.binary.op == AST_BINOP_NE);

                if (!l_is_ptr || !r_is_ptr) {
                    if (!is_rel && ((l_is_ptr && r_null) || (r_is_ptr && l_null))) {} else {
                        scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Invalid comparison between pointer and non-pointer");
                    }
                } else {
                    Type* lb = clt ? clt->base : 0;
                    Type* rb = crt ? crt->base : 0;
                    if (!lb || !rb) {
                        scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid pointer type");
                    }

                    if (is_rel) {
                        if (l_null || r_null) {
                            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Relational comparison with null pointer constant is not allowed");
                        }
                        if (lb->kind == TYPE_VOID || rb->kind == TYPE_VOID) {
                            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Relational comparison on void* is not allowed");
                        }
                        if (!type_compatible_unqualified(lb, rb)) {
                            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Relational comparison requires compatible pointer types");
                        }
                    } else {
                        if (!(lb->kind == TYPE_VOID || rb->kind == TYPE_VOID || type_compatible_unqualified(lb, rb))) {
                            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Comparison requires compatible pointer types");
                        }
                    }
                }
            } else {
                if (!ir_tc_is_int_type(clt) || !ir_tc_is_int_type(crt)) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Comparison requires integer operands");
                }
            }

            int is_unsigned = 0;
            if (!is_ptr) {
                Type* ct = ir_tc_uac_common_int_type(fc->lc, clt, crt);
                if (!ct) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid comparison type");
                }
                is_unsigned = ir_tc_uac_is_unsigned_kind(ct->kind);
            } else {
                is_unsigned = 1;
            }
            IrType* ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;

            IrValueId lv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.left), ity, e->tok);
            IrValueId rv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.right), ity, e->tok);

            IrIcmpPred p = IR_ICMP_EQ;
            if (e->v.binary.op == AST_BINOP_EQ) p = IR_ICMP_EQ;
            else if (e->v.binary.op == AST_BINOP_NE) p = IR_ICMP_NE;
            else if (e->v.binary.op == AST_BINOP_LT) p = is_unsigned ? IR_ICMP_ULT : IR_ICMP_SLT;
            else if (e->v.binary.op == AST_BINOP_LE) p = is_unsigned ? IR_ICMP_ULE : IR_ICMP_SLE;
            else if (e->v.binary.op == AST_BINOP_GT) p = is_unsigned ? IR_ICMP_UGT : IR_ICMP_SGT;
            else if (e->v.binary.op == AST_BINOP_GE) p = is_unsigned ? IR_ICMP_UGE : IR_ICMP_SGE;
            return ir_emit_icmp(fc->f, fc->cur, p, lv, rv);
        }

        if (e->v.binary.op == AST_BINOP_ADD || e->v.binary.op == AST_BINOP_SUB) {
            if ((lt && lt->kind == TYPE_VOID) || (rt && rt->kind == TYPE_VOID)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Void value is not allowed here");
            }
            int is_unsigned = 0;
            IrType* ity = fc->f->ty_i32;

            if (!lptr && !rptr) {
                if (!ir_tc_is_int_type(lt) || !ir_tc_is_int_type(rt)) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Arithmetic operator requires integer operands");
                }

                Type* ct = ir_tc_uac_common_int_type(fc->lc, lt, rt);
                if (!ct) {
                    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid arithmetic type");
                }
                is_unsigned = ir_tc_uac_is_unsigned_kind(ct->kind);
                ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;
            }

            IrValueId lv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.left), ity, e->tok);
            IrValueId rv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.right), ity, e->tok);
            IrInstrKind k = (e->v.binary.op == AST_BINOP_ADD) ? IR_INSTR_ADD : IR_INSTR_SUB;
            return ir_emit_bin(fc->f, fc->cur, k, ity, lv, rv);
        }

        if (e->v.binary.op == AST_BINOP_BAND || e->v.binary.op == AST_BINOP_BXOR || e->v.binary.op == AST_BINOP_BOR) {
            if ((lt && lt->kind == TYPE_VOID) || (rt && rt->kind == TYPE_VOID)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Void value is not allowed here");
            }
            if (!ir_tc_is_int_type(lt) || !ir_tc_is_int_type(rt)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Bitwise operator requires integer operands");
            }

            Type* ct = ir_tc_uac_common_int_type(fc->lc, lt, rt);
            if (!ct) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid arithmetic type");
            }
            int is_unsigned = ir_tc_uac_is_unsigned_kind(ct->kind);
            IrType* ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;

            IrValueId lv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.left), ity, e->tok);
            IrValueId rv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.right), ity, e->tok);

            IrInstrKind k = IR_INSTR_INVALID;
            if (e->v.binary.op == AST_BINOP_BAND) k = IR_INSTR_AND;
            else if (e->v.binary.op == AST_BINOP_BOR) k = IR_INSTR_OR;
            else if (e->v.binary.op == AST_BINOP_BXOR) k = IR_INSTR_XOR;
            return ir_emit_bin(fc->f, fc->cur, k, ity, lv, rv);
        }

        if (e->v.binary.op == AST_BINOP_SHL || e->v.binary.op == AST_BINOP_SHR) {
            if ((lt && lt->kind == TYPE_VOID) || (rt && rt->kind == TYPE_VOID)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Void value is not allowed here");
            }
            if (!ir_tc_is_int_type(lt) || !ir_tc_is_int_type(rt)) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Shift operator requires integer operands");
            }

            Type* pt = ir_tc_uac_promote_int_type(fc->lc, lt);
            if (!pt) {
                scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Internal error: invalid shift type");
            }
            int is_unsigned = ir_tc_uac_is_unsigned_kind(pt->kind);
            IrType* ity = is_unsigned ? fc->f->ty_u32 : fc->f->ty_i32;

            IrValueId lv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.left), ity, e->tok);
            IrValueId rv = ir_lower_cast_value(fc, ir_lower_expr(fc, e->v.binary.right), ity, e->tok);

            IrInstrKind k = IR_INSTR_INVALID;
            if (e->v.binary.op == AST_BINOP_SHL) k = IR_INSTR_SHL;
            else k = is_unsigned ? IR_INSTR_SHR : IR_INSTR_SAR;
            return ir_emit_bin(fc->f, fc->cur, k, ity, lv, rv);
        }

        scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Binary operator not lowered to IR yet");
    }

    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, e->tok.line, e->tok.col, "Expression kind is not lowered to IR yet");
    return 0;
}

static IrValueId ir_lower_expr_bool(IrLowerFuncCtx* fc, AstExpr* e, Token tok) {
    if (!fc || !fc->f) return 0;

    Type* t = ir_lower_expr_type(fc->lc, e);
    if (t && t->kind == TYPE_VOID) {
        scc_fatal_at(fc->lc->p->file, fc->lc->p->src, tok.line, tok.col, "Void value is not allowed here");
    }
    if (!ir_tc_is_scalar_type(t)) {
        scc_fatal_at(fc->lc->p->file, fc->lc->p->src, tok.line, tok.col, "Condition must have scalar type");
    }

    IrValueId v = ir_lower_expr(fc, e);
    return ir_lower_cast_value(fc, v, fc->f->ty_bool, tok);
}

static void ir_lower_stmt_list(IrLowerFuncCtx* fc, AstStmt* first);

static void ir_lower_stmt_list(IrLowerFuncCtx* fc, AstStmt* first) {
    if (!fc) return;
    for (AstStmt* it = first; it; it = it->next) {
        if (ir_block_is_terminated(fc->f, fc->cur)) return;
        ir_lower_stmt(fc, it);
    }
}

static void ir_lower_stmt(IrLowerFuncCtx* fc, AstStmt* s);

static void ir_lower_stmt(IrLowerFuncCtx* fc, AstStmt* s) {
    if (!fc || !fc->lc || !fc->f || !s) return;

    if (s->kind == AST_STMT_BLOCK) {
        ir_lower_stmt_list(fc, s->v.block.first);
        return;
    }

    if (s->kind == AST_STMT_DECL) {
        IrValueId addr = ir_lower_get_var_addr(fc, s->v.decl.decl_var);
        if (s->v.decl.init) {
            ir_tc_check_assign(fc->lc, s->tok, s->v.decl.decl_type, s->v.decl.init);
            IrValueId iv = ir_lower_expr(fc, s->v.decl.init);
            IrType* ty = ir_type_from_scc(fc->f, s->v.decl.decl_type);
            IrValueId cv = ir_lower_cast_value(fc, iv, ty, s->tok);
            ir_emit_store(fc->f, fc->cur, addr, cv);
        }
        (void)addr;
        return;
    }

    if (s->kind == AST_STMT_EXPR) {
        if (s->v.expr.expr) (void)ir_lower_expr(fc, s->v.expr.expr);
        return;
    }

    if (s->kind == AST_STMT_RETURN) {
        IrValueId rv = 0;
        if (fc->f->ret_type && fc->f->ret_type->kind != IR_TY_VOID) {
            if (s->v.expr.expr) {
                ir_tc_check_assign(fc->lc, s->tok, fc->af && fc->af->sym ? fc->af->sym->ftype.ret : 0, s->v.expr.expr);
                rv = ir_lower_expr(fc, s->v.expr.expr);
            } else {
                rv = ir_emit_iconst(fc->f, fc->cur, 0);
            }
            rv = ir_lower_cast_value(fc, rv, fc->f->ret_type, s->tok);
        }
        ir_set_term_ret(fc->f, fc->cur, rv);
        return;
    }

    if (s->kind == AST_STMT_BREAK) {
        if (!fc->loops) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, s->tok.line, s->tok.col, "break not within loop");
        }
        ir_set_term_br(fc->f, fc->cur, fc->loops->break_target, 0, 0);
        return;
    }

    if (s->kind == AST_STMT_CONTINUE) {
        if (!fc->loops) {
            scc_fatal_at(fc->lc->p->file, fc->lc->p->src, s->tok.line, s->tok.col, "continue not within loop");
        }
        ir_set_term_br(fc->f, fc->cur, fc->loops->continue_target, 0, 0);
        return;
    }

    if (s->kind == AST_STMT_IF) {
        IrValueId cond = ir_lower_expr_bool(fc, s->v.if_stmt.cond, s->tok);

        IrBlockId then_b = ir_block_new(fc->f);
        IrBlockId end_b = ir_block_new(fc->f);
        IrBlockId else_b = s->v.if_stmt.else_stmt ? ir_block_new(fc->f) : end_b;

        ir_set_term_condbr(fc->f, fc->cur, cond, then_b, 0, 0, else_b, 0, 0);

        fc->cur = then_b;
        ir_lower_stmt(fc, s->v.if_stmt.then_stmt);
        if (!ir_block_is_terminated(fc->f, fc->cur)) {
            ir_set_term_br(fc->f, fc->cur, end_b, 0, 0);
        }

        if (s->v.if_stmt.else_stmt) {
            fc->cur = else_b;
            ir_lower_stmt(fc, s->v.if_stmt.else_stmt);
            if (!ir_block_is_terminated(fc->f, fc->cur)) {
                ir_set_term_br(fc->f, fc->cur, end_b, 0, 0);
            }
        }

        fc->cur = end_b;
        return;
    }

    if (s->kind == AST_STMT_WHILE) {
        IrBlockId cond_b = ir_block_new(fc->f);
        IrBlockId body_b = ir_block_new(fc->f);
        IrBlockId exit_b = ir_block_new(fc->f);

        ir_set_term_br(fc->f, fc->cur, cond_b, 0, 0);

        fc->cur = cond_b;
        IrValueId cond = ir_lower_expr_bool(fc, s->v.while_stmt.cond, s->tok);
        ir_set_term_condbr(fc->f, fc->cur, cond, body_b, 0, 0, exit_b, 0, 0);

        fc->cur = body_b;
        IrLowerLoop loop;
        loop.break_target = exit_b;
        loop.continue_target = cond_b;
        loop.next = fc->loops;
        fc->loops = &loop;

        ir_lower_stmt(fc, s->v.while_stmt.body);

        fc->loops = loop.next;
        if (!ir_block_is_terminated(fc->f, fc->cur)) {
            ir_set_term_br(fc->f, fc->cur, cond_b, 0, 0);
        }

        fc->cur = exit_b;
        return;
    }

    scc_fatal_at(fc->lc->p->file, fc->lc->p->src, s->tok.line, s->tok.col, "Statement kind is not lowered to IR yet");
}

static void ir_lower_func_signature(IrLowerCtx* lc, IrFunc* f, Symbol* sym) {
    if (!lc || !f || !sym) return;

    f->ret_type = ir_type_from_scc(f, sym->ftype.ret);
    f->param_count = (uint32_t)sym->ftype.param_count;

    if (f->param_count) {
        IrType** pts = (IrType**)arena_alloc(f->arena, f->param_count * sizeof(IrType*), 8);
        for (uint32_t i = 0; i < f->param_count; i++) {
            Type* st = sym->ftype.params ? sym->ftype.params[i] : 0;
            pts[i] = ir_type_from_scc(f, st);
        }
        f->param_types = pts;
    }
}

static void ir_lower_func_stub_body(IrLowerCtx* lc, IrFunc* f, AstFunc* af) {
    if (!lc || !f || !af) return;

    IrBlockId entry = ir_block_new(f);
    f->entry = entry;

    for (uint32_t i = 0; i < f->param_count; i++) {
        (void)ir_block_add_param(f, entry, f->param_types[i]);
    }

    IrLowerFuncCtx fc;
    memset(&fc, 0, sizeof(fc));
    fc.lc = lc;
    fc.f = f;
    fc.af = af;
    fc.cur = entry;
    fc.vars = 0;
    fc.loops = 0;

    ir_lower_stmt_list(&fc, af->first_stmt);

    if (!ir_block_is_terminated(f, fc.cur)) {
        IrValueId rv = 0;
        if (f->ret_type && f->ret_type->kind != IR_TY_VOID) {
            rv = ir_emit_iconst(f, fc.cur, 0);
            rv = ir_lower_cast_value(&fc, rv, f->ret_type, af->first_stmt ? af->first_stmt->tok : (Token){0});
        }
        ir_set_term_ret(f, fc.cur, rv);
    }
}

void ir_lower_unit_stub(IrModule* m, Parser* p, SymTable* syms, Buffer* data, uint32_t* io_str_id, AstUnit* u) {
    IrLowerCtx lc;
    memset(&lc, 0, sizeof(lc));
    lc.p = p;
    lc.syms = syms;
    lc.m = m;
    lc.data = data;
    lc.str_id = io_str_id ? *io_str_id : 0;

    for (AstFunc* af = u ? u->first_func : 0; af; af = af->next) {
        if (!af->sym) continue;
        if (af->sym->kind != SYM_FUNC) continue;
        if (af->sym->shndx == SHN_UNDEF) continue;

        IrFunc* f = ir_func_new(m, af->sym);
        ir_lower_func_signature(&lc, f, af->sym);
        ir_lower_func_stub_body(&lc, f, af);
    }

    if (io_str_id) *io_str_id = lc.str_id;
}

#undef SCC_IR_LOWER_IMPLEMENTATION

#endif

#endif

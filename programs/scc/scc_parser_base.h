// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_PARSER_BASE_H_INCLUDED
#define SCC_PARSER_BASE_H_INCLUDED

#include "scc_ast.h"

typedef struct {
    const char* file;
    const char* src;
    Lexer lx;
    Token tok;
    Arena* arena;
    SymTable* syms;

    Var* scope_vars;
    ScopeFrame* scope_frames;
    int scope_local_size;
    int scope_param_count;
    int loop_depth;
} Parser;

void parser_next(Parser* p);
void parser_expect(Parser* p, TokenKind k, const char* msg);
int parser_match(Parser* p, TokenKind k);
AstExpr* ast_new_expr(Parser* p, AstExprKind kind, Token tok);
AstStmt* ast_new_stmt(Parser* p, AstStmtKind kind, Token tok);
int tok_to_binop(TokenKind k, AstBinOp* out_op, int* out_prec, int* out_right_assoc);

Type* type_int(Parser* p);
Type* type_uint(Parser* p);
Type* type_short(Parser* p);
Type* type_ushort(Parser* p);
Type* type_long(Parser* p);
Type* type_ulong(Parser* p);
Type* type_char(Parser* p);
Type* type_uchar(Parser* p);
Type* type_bool(Parser* p);
Type* type_void(Parser* p);
Type* type_ptr_to(Parser* p, Type* base);
Type* parse_type(Parser* p);

Var* scope_find(Parser* p, const char* name);
Var* scope_find_current(Parser* p, const char* name);
void scope_enter(Parser* p);
void scope_leave(Parser* p);
Var* scope_add_param(Parser* p, const char* name, Type* ty, int index);
Var* scope_add_local(Parser* p, const char* name, Type* ty);
char* decode_string(Parser* p, Token t, int* out_len);

#ifdef SCC_PARSER_BASE_IMPLEMENTATION

void parser_next(Parser* p) {
    p->tok = lx_next(&p->lx);
}

void parser_expect(Parser* p, TokenKind k, const char* msg) {
    if (p->tok.kind != k) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, msg);
    }
    parser_next(p);
}

int parser_match(Parser* p, TokenKind k) {
    if (p->tok.kind != k) return 0;
    parser_next(p);
    return 1;
}

AstExpr* ast_new_expr(Parser* p, AstExprKind kind, Token tok) {
    AstExpr* e = (AstExpr*)arena_alloc(p->arena, sizeof(AstExpr), 8);
    e->kind = kind;
    e->tok = tok;
    return e;
}

AstStmt* ast_new_stmt(Parser* p, AstStmtKind kind, Token tok) {
    AstStmt* s = (AstStmt*)arena_alloc(p->arena, sizeof(AstStmt), 8);
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->tok = tok;
    return s;
}

int tok_to_binop(TokenKind k, AstBinOp* out_op, int* out_prec, int* out_right_assoc) {
    *out_right_assoc = 0;

    if (k == TOK_STAR) { *out_op = AST_BINOP_MUL; *out_prec = 60; return 1; }
    if (k == TOK_SLASH) { *out_op = AST_BINOP_DIV; *out_prec = 60; return 1; }
    if (k == TOK_PERCENT) { *out_op = AST_BINOP_MOD; *out_prec = 60; return 1; }
    if (k == TOK_PLUS) { *out_op = AST_BINOP_ADD; *out_prec = 50; return 1; }
    if (k == TOK_MINUS) { *out_op = AST_BINOP_SUB; *out_prec = 50; return 1; }

    if (k == TOK_LSHIFT) { *out_op = AST_BINOP_SHL; *out_prec = 45; return 1; }
    if (k == TOK_RSHIFT) { *out_op = AST_BINOP_SHR; *out_prec = 45; return 1; }

    if (k == TOK_LT) { *out_op = AST_BINOP_LT; *out_prec = 40; return 1; }
    if (k == TOK_LE) { *out_op = AST_BINOP_LE; *out_prec = 40; return 1; }
    if (k == TOK_GT) { *out_op = AST_BINOP_GT; *out_prec = 40; return 1; }
    if (k == TOK_GE) { *out_op = AST_BINOP_GE; *out_prec = 40; return 1; }

    if (k == TOK_EQ) { *out_op = AST_BINOP_EQ; *out_prec = 35; return 1; }
    if (k == TOK_NE) { *out_op = AST_BINOP_NE; *out_prec = 35; return 1; }

    if (k == TOK_AMP) { *out_op = AST_BINOP_BAND; *out_prec = 34; return 1; }
    if (k == TOK_CARET) { *out_op = AST_BINOP_BXOR; *out_prec = 33; return 1; }
    if (k == TOK_PIPE) { *out_op = AST_BINOP_BOR; *out_prec = 32; return 1; }

    if (k == TOK_ANDAND) { *out_op = AST_BINOP_ANDAND; *out_prec = 30; return 1; }
    if (k == TOK_OROR) { *out_op = AST_BINOP_OROR; *out_prec = 25; return 1; }

    return 0;
}

Type* type_int(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_INT;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_uint(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_UINT;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_short(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_SHORT;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_ushort(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_USHORT;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_long(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_LONG;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_ulong(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_ULONG;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_char(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_CHAR;
    t->base = 0;
    t->is_const = 0;
    return t;
}

 Type* type_uchar(Parser* p) {
     Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
     t->kind = TYPE_UCHAR;
     t->base = 0;
     t->is_const = 0;
     return t;
 }

Type* type_bool(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_BOOL;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_void(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_VOID;
    t->base = 0;
    t->is_const = 0;
    return t;
}

Type* type_ptr_to(Parser* p, Type* base) {
    Type* pt = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    pt->kind = TYPE_PTR;
    pt->base = base;
    pt->is_const = 0;
    return pt;
}

Type* parse_type(Parser* p) {
    int saw_const = 0;
    while (parser_match(p, TOK_KW_CONST)) saw_const = 1;

    int saw_any = 0;
    int saw_signed = 0;
    int saw_unsigned = 0;
    int short_count = 0;
    int long_count = 0;
    int saw_int = 0;
    int saw_char = 0;
    int saw_bool = 0;
    int saw_void = 0;

    while (1) {
        if (p->tok.kind == TOK_KW_SIGNED) {
            saw_signed = 1;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_UNSIGNED) {
            saw_unsigned = 1;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_SHORT) {
            short_count++;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_LONG) {
            long_count++;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_INT) {
            saw_int = 1;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_CHAR) {
            saw_char = 1;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_BOOL) {
            saw_bool = 1;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        if (p->tok.kind == TOK_KW_VOID) {
            saw_void = 1;
            saw_any = 1;
            parser_next(p);
            continue;
        }
        break;
    }

    if (!saw_any) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected type name");
    }

    if (saw_signed && saw_unsigned) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: both signed and unsigned");
    }
    if (short_count > 1) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: repeated short");
    }
    if (long_count > 1) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: long long is not supported");
    }

    Type* base = 0;
    if (saw_void) {
        if (saw_char || saw_bool || saw_int || short_count || long_count || saw_signed || saw_unsigned) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: void with other specifiers");
        }
        base = type_void(p);
    } else if (saw_bool) {
        if (saw_char || saw_int || short_count || long_count || saw_signed || saw_unsigned) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: _Bool with other specifiers");
        }
        base = type_bool(p);
    } else if (saw_char) {
        if (saw_int || short_count || long_count) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: char with integer width specifiers");
        }
        if (saw_unsigned) base = type_uchar(p);
        else base = type_char(p);
    } else {
        if (short_count && long_count) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Invalid type: short with long");
        }

        if (short_count) base = saw_unsigned ? type_ushort(p) : type_short(p);
        else if (long_count) base = saw_unsigned ? type_ulong(p) : type_long(p);
        else base = saw_unsigned ? type_uint(p) : type_int(p);
    }

    if (!base) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected type name");
    }

    if (saw_const) base->is_const = 1;

    while (parser_match(p, TOK_STAR)) {
        base = type_ptr_to(p, base);
    }

    return base;
}

Var* scope_find(Parser* p, const char* name) {
    for (Var* v = p->scope_vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return 0;
}

Var* scope_find_current(Parser* p, const char* name) {
    Var* stop = 0;
    if (p->scope_frames) stop = p->scope_frames->prev_vars;
    for (Var* v = p->scope_vars; v && v != stop; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return 0;
}

void scope_enter(Parser* p) {
    ScopeFrame* f = (ScopeFrame*)arena_alloc(p->arena, sizeof(ScopeFrame), 8);
    memset(f, 0, sizeof(*f));
    f->prev_vars = p->scope_vars;
    f->next = p->scope_frames;
    p->scope_frames = f;
}

void scope_leave(Parser* p) {
    if (!p->scope_frames) return;
    p->scope_vars = p->scope_frames->prev_vars;
    p->scope_frames = p->scope_frames->next;
}

Var* scope_add_param(Parser* p, const char* name, Type* ty, int index) {
    if (!name) return 0;
    if (scope_find_current(p, name)) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Duplicate parameter name");
    }
    Var* v = (Var*)arena_alloc(p->arena, sizeof(Var), 8);
    memset(v, 0, sizeof(*v));
    v->name = arena_strndup(p->arena, name, (int)strlen(name));
    v->ty = ty;
    v->kind = VAR_PARAM;
    v->ebp_offset = 8 + index * 4;
    v->next = p->scope_vars;
    p->scope_vars = v;
    return v;
}

Var* scope_add_local(Parser* p, const char* name, Type* ty) {
    if (scope_find_current(p, name)) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Duplicate local name");
    }
    p->scope_local_size += 4;
    Var* v = (Var*)arena_alloc(p->arena, sizeof(Var), 8);
    memset(v, 0, sizeof(*v));
    v->name = arena_strndup(p->arena, name, (int)strlen(name));
    v->ty = ty;
    v->kind = VAR_LOCAL;
    v->ebp_offset = -p->scope_local_size;
    v->next = p->scope_vars;
    p->scope_vars = v;
    return v;
}

char* decode_string(Parser* p, Token t, int* out_len) {
    const char* s = t.begin;
    const char* e = t.begin + t.len;
    char* out = (char*)arena_alloc(p->arena, (uint32_t)t.len + 1, 1);
    int n = 0;
    while (s < e) {
        char c = *s++;
        if (c == '\\') {
            if (s >= e) scc_fatal_at(p->file, p->src, t.line, t.col, "Invalid escape in string literal");
            char esc = *s++;
            if (esc == 'n') out[n++] = '\n';
            else if (esc == 't') out[n++] = '\t';
            else if (esc == 'r') out[n++] = '\r';
            else if (esc == '0') out[n++] = 0;
            else if (esc == '\\') out[n++] = '\\';
            else if (esc == '"') out[n++] = '"';
            else scc_fatal_at(p->file, p->src, t.line, t.col, "Unsupported escape in string literal");
            continue;
        }
        out[n++] = c;
    }
    out[n] = 0;
    *out_len = n;
    return out;
}

#endif

#endif

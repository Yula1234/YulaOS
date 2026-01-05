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

static void parser_next(Parser* p) {
    p->tok = lx_next(&p->lx);
}

static void parser_expect(Parser* p, TokenKind k, const char* msg) {
    if (p->tok.kind != k) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, msg);
    }
    parser_next(p);
}

static int parser_match(Parser* p, TokenKind k) {
    if (p->tok.kind != k) return 0;
    parser_next(p);
    return 1;
}

static AstExpr* ast_new_expr(Parser* p, AstExprKind kind, Token tok) {
    AstExpr* e = (AstExpr*)arena_alloc(p->arena, sizeof(AstExpr), 8);
    e->kind = kind;
    e->tok = tok;
    return e;
}

static AstStmt* ast_new_stmt(Parser* p, AstStmtKind kind, Token tok) {
    AstStmt* s = (AstStmt*)arena_alloc(p->arena, sizeof(AstStmt), 8);
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->tok = tok;
    return s;
}

static int tok_to_binop(TokenKind k, AstBinOp* out_op, int* out_prec, int* out_right_assoc) {
    *out_right_assoc = 0;

    if (k == TOK_STAR) { *out_op = AST_BINOP_MUL; *out_prec = 60; return 1; }
    if (k == TOK_SLASH) { *out_op = AST_BINOP_DIV; *out_prec = 60; return 1; }
    if (k == TOK_PERCENT) { *out_op = AST_BINOP_MOD; *out_prec = 60; return 1; }
    if (k == TOK_PLUS) { *out_op = AST_BINOP_ADD; *out_prec = 50; return 1; }
    if (k == TOK_MINUS) { *out_op = AST_BINOP_SUB; *out_prec = 50; return 1; }

    if (k == TOK_LT) { *out_op = AST_BINOP_LT; *out_prec = 40; return 1; }
    if (k == TOK_LE) { *out_op = AST_BINOP_LE; *out_prec = 40; return 1; }
    if (k == TOK_GT) { *out_op = AST_BINOP_GT; *out_prec = 40; return 1; }
    if (k == TOK_GE) { *out_op = AST_BINOP_GE; *out_prec = 40; return 1; }

    if (k == TOK_EQ) { *out_op = AST_BINOP_EQ; *out_prec = 35; return 1; }
    if (k == TOK_NE) { *out_op = AST_BINOP_NE; *out_prec = 35; return 1; }

    if (k == TOK_ANDAND) { *out_op = AST_BINOP_ANDAND; *out_prec = 30; return 1; }
    if (k == TOK_OROR) { *out_op = AST_BINOP_OROR; *out_prec = 25; return 1; }

    return 0;
}

static AstExpr* parse_expr_prec(Parser* p, int min_prec);
static AstExpr* parse_expr(Parser* p);

static Type* type_int(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_INT;
    t->base = 0;
    t->is_const = 0;
    return t;
}

static Type* type_char(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_CHAR;
    t->base = 0;
    t->is_const = 0;
    return t;
}

static Type* type_bool(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_BOOL;
    t->base = 0;
    t->is_const = 0;
    return t;
}

static Type* type_void(Parser* p) {
    Type* t = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    t->kind = TYPE_VOID;
    t->base = 0;
    t->is_const = 0;
    return t;
}

static Type* type_ptr_to(Parser* p, Type* base) {
    Type* pt = (Type*)arena_alloc(p->arena, sizeof(Type), 8);
    pt->kind = TYPE_PTR;
    pt->base = base;
    pt->is_const = 0;
    return pt;
}

static Type* parse_type(Parser* p) {
    int saw_const = 0;
    while (parser_match(p, TOK_KW_CONST)) saw_const = 1;

    Type* base = 0;
    if (p->tok.kind == TOK_KW_INT) {
        base = type_int(p);
        parser_next(p);
    } else if (p->tok.kind == TOK_KW_CHAR) {
        base = type_char(p);
        parser_next(p);
    } else if (p->tok.kind == TOK_KW_BOOL) {
        base = type_bool(p);
        parser_next(p);
    } else if (p->tok.kind == TOK_KW_VOID) {
        base = type_void(p);
        parser_next(p);
    } else {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected type name");
    }

    if (saw_const) base->is_const = 1;

    while (parser_match(p, TOK_STAR)) {
        base = type_ptr_to(p, base);
    }

    return base;
}

static Var* scope_find(Parser* p, const char* name) {
    for (Var* v = p->scope_vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return 0;
}

static Var* scope_find_current(Parser* p, const char* name) {
    Var* stop = 0;
    if (p->scope_frames) stop = p->scope_frames->prev_vars;
    for (Var* v = p->scope_vars; v && v != stop; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return 0;
}

static void scope_enter(Parser* p) {
    ScopeFrame* f = (ScopeFrame*)arena_alloc(p->arena, sizeof(ScopeFrame), 8);
    memset(f, 0, sizeof(*f));
    f->prev_vars = p->scope_vars;
    f->next = p->scope_frames;
    p->scope_frames = f;
}

static void scope_leave(Parser* p) {
    if (!p->scope_frames) return;
    p->scope_vars = p->scope_frames->prev_vars;
    p->scope_frames = p->scope_frames->next;
}

static Var* scope_add_param(Parser* p, const char* name, Type* ty, int index) {
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

static Var* scope_add_local(Parser* p, const char* name, Type* ty) {
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

static char* decode_string(Parser* p, Token t, int* out_len) {
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

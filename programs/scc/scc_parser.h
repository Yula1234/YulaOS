#ifndef SCC_PARSER_H_INCLUDED
#define SCC_PARSER_H_INCLUDED

#include "scc_parser_base.h"

static AstExpr* parse_primary(Parser* p) {
    if (p->tok.kind == TOK_NUM) {
        Token t = p->tok;
        AstExpr* e = ast_new_expr(p, AST_EXPR_INT_LIT, t);
        e->v.int_lit = t.num_i32;
        parser_next(p);
        return e;
    }

    if (p->tok.kind == TOK_STR) {
        Token t = p->tok;
        AstExpr* e = ast_new_expr(p, AST_EXPR_STR, t);
        int slen = 0;
        e->v.str.bytes = decode_string(p, t, &slen);
        e->v.str.len = slen;
        parser_next(p);
        return e;
    }

    if (p->tok.kind == TOK_IDENT) {
        Token t = p->tok;
        AstExpr* e = ast_new_expr(p, AST_EXPR_NAME, t);
        e->v.name.name = arena_strndup(p->arena, t.begin, t.len);
        parser_next(p);
        return e;
    }

    if (parser_match(p, TOK_LPAREN)) {
        AstExpr* e = parse_expr_prec(p, 0);
        parser_expect(p, TOK_RPAREN, "Expected ')' after expression");
        return e;
    }

    scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected expression");
    return 0;
}

static AstExpr* parse_postfix(Parser* p) {
    AstExpr* e = parse_primary(p);

    while (p->tok.kind == TOK_LPAREN) {
        Token t = p->tok;
        parser_next(p);

        AstExpr* tmp_args[32];
        int argc = 0;

        if (p->tok.kind != TOK_RPAREN) {
            while (1) {
                if (argc >= (int)(sizeof(tmp_args) / sizeof(tmp_args[0]))) {
                    scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Too many call arguments");
                }
                tmp_args[argc++] = parse_expr(p);
                if (!parser_match(p, TOK_COMMA)) break;
            }
        }

        parser_expect(p, TOK_RPAREN, "Expected ')' after call arguments");

        if (e->kind != AST_EXPR_NAME) {
            scc_fatal_at(p->file, p->src, t.line, t.col, "Call of non-identifier is not supported");
        }

        AstExpr* call = ast_new_expr(p, AST_EXPR_CALL, t);
        call->v.call.callee = e->v.name.name;
        call->v.call.arg_count = argc;
        if (argc) {
            call->v.call.args = (AstExpr**)arena_alloc(p->arena, (uint32_t)argc * (uint32_t)sizeof(AstExpr*), 8);
            for (int i = 0; i < argc; i++) call->v.call.args[i] = tmp_args[i];
        } else {
            call->v.call.args = 0;
        }
        e = call;
    }

    if (e->kind == AST_EXPR_NAME) {
        Var* v = scope_find(p, e->v.name.name);
        if (v) {
            e->v.name.var = v;
            e->v.name.sym = 0;
            return e;
        }

        Symbol* s = symtab_find(p->syms, e->v.name.name);
        if (s && s->kind == SYM_DATA) {
            e->v.name.var = 0;
            e->v.name.sym = s;
            return e;
        }

        e->v.name.var = 0;
        e->v.name.sym = 0;
    }

    return e;
}

static AstExpr* parse_unary(Parser* p) {
    if (p->tok.kind == TOK_LPAREN) {
        Lexer snap_lx = p->lx;
        Token snap_tok = p->tok;

        Token t = p->tok;
        parser_next(p);
        if (p->tok.kind == TOK_KW_CONST || p->tok.kind == TOK_KW_INT || p->tok.kind == TOK_KW_CHAR || p->tok.kind == TOK_KW_BOOL || p->tok.kind == TOK_KW_VOID) {
            Type* ty = parse_type(p);
            parser_expect(p, TOK_RPAREN, "Expected ')' after cast type");

            AstExpr* inner = parse_unary(p);
            AstExpr* e = ast_new_expr(p, AST_EXPR_CAST, t);
            e->v.cast.ty = ty;
            e->v.cast.expr = inner;
            return e;
        }

        p->lx = snap_lx;
        p->tok = snap_tok;
    }

    if (p->tok.kind == TOK_BANG) {
        Token t = p->tok;
        parser_next(p);

        AstExpr* e = ast_new_expr(p, AST_EXPR_UNARY, t);
        e->v.unary.op = AST_UNOP_NOT;
        e->v.unary.expr = parse_unary(p);
        return e;
    }

    if (p->tok.kind == TOK_AMP || p->tok.kind == TOK_STAR) {
        Token t = p->tok;
        AstUnOp op = (p->tok.kind == TOK_AMP) ? AST_UNOP_ADDR : AST_UNOP_DEREF;
        parser_next(p);

        AstExpr* e = ast_new_expr(p, AST_EXPR_UNARY, t);
        e->v.unary.op = op;
        e->v.unary.expr = parse_unary(p);
        return e;
    }

    if (p->tok.kind == TOK_PLUS || p->tok.kind == TOK_MINUS) {
        Token t = p->tok;
        AstUnOp op = (p->tok.kind == TOK_MINUS) ? AST_UNOP_NEG : AST_UNOP_POS;
        parser_next(p);

        AstExpr* e = ast_new_expr(p, AST_EXPR_UNARY, t);
        e->v.unary.op = op;
        e->v.unary.expr = parse_unary(p);
        return e;
    }

    return parse_postfix(p);
}

static int expr_is_lvalue(AstExpr* e) {
    if (!e) return 0;
    if (e->kind == AST_EXPR_NAME) return 1;
    if (e->kind == AST_EXPR_UNARY && e->v.unary.op == AST_UNOP_DEREF) return 1;
    return 0;
}

static AstExpr* parse_expr_prec(Parser* p, int min_prec) {
    AstExpr* lhs = parse_unary(p);

    while (1) {
        if (p->tok.kind == TOK_ASSIGN) {
            int prec = 10;
            int right_assoc = 1;
            if (prec < min_prec) break;
            Token t = p->tok;
            parser_next(p);
            int next_min = right_assoc ? prec : (prec + 1);
            AstExpr* rhs = parse_expr_prec(p, next_min);
            if (!expr_is_lvalue(lhs)) {
                scc_fatal_at(p->file, p->src, t.line, t.col, "Left-hand side of assignment must be an assignable expression");
            }
            AstExpr* e = ast_new_expr(p, AST_EXPR_ASSIGN, t);
            e->v.assign.left = lhs;
            e->v.assign.right = rhs;
            lhs = e;
            continue;
        }

        AstBinOp op;
        int prec = 0;
        int right_assoc = 0;
        if (!tok_to_binop(p->tok.kind, &op, &prec, &right_assoc)) break;
        if (prec < min_prec) break;

        Token t = p->tok;
        parser_next(p);

        int next_min = right_assoc ? prec : (prec + 1);
        AstExpr* rhs = parse_expr_prec(p, next_min);

        AstExpr* e = ast_new_expr(p, AST_EXPR_BINARY, t);
        e->v.binary.op = op;
        e->v.binary.left = lhs;
        e->v.binary.right = rhs;
        lhs = e;
    }

    return lhs;
}

static AstExpr* parse_expr(Parser* p) {
    return parse_expr_prec(p, 0);
}

static AstStmt* parse_stmt(Parser* p) {
    if (p->tok.kind == TOK_LBRACE) {
        Token t = p->tok;
        parser_next(p);

        scope_enter(p);
        AstStmt* first = 0;
        AstStmt* last = 0;
        while (p->tok.kind != TOK_RBRACE) {
            if (p->tok.kind == TOK_EOF) {
                scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Unexpected end of file in block");
            }
            AstStmt* s = parse_stmt(p);
            if (!first) first = s;
            else last->next = s;
            last = s;
        }
        parser_expect(p, TOK_RBRACE, "Expected '}' after block");
        scope_leave(p);

        AstStmt* b = ast_new_stmt(p, AST_STMT_BLOCK, t);
        b->v.block.first = first;
        return b;
    }

    if (p->tok.kind == TOK_KW_IF) {
        Token t = p->tok;
        parser_next(p);
        parser_expect(p, TOK_LPAREN, "Expected '(' after if");
        AstExpr* cond = parse_expr(p);
        parser_expect(p, TOK_RPAREN, "Expected ')' after if condition");
        AstStmt* then_stmt = parse_stmt(p);
        AstStmt* else_stmt = 0;
        if (parser_match(p, TOK_KW_ELSE)) {
            else_stmt = parse_stmt(p);
        }
        AstStmt* s = ast_new_stmt(p, AST_STMT_IF, t);
        s->v.if_stmt.cond = cond;
        s->v.if_stmt.then_stmt = then_stmt;
        s->v.if_stmt.else_stmt = else_stmt;
        return s;
    }

    if (p->tok.kind == TOK_KW_WHILE) {
        Token t = p->tok;
        parser_next(p);
        parser_expect(p, TOK_LPAREN, "Expected '(' after while");
        AstExpr* cond = parse_expr(p);
        parser_expect(p, TOK_RPAREN, "Expected ')' after while condition");

        p->loop_depth++;
        AstStmt* body = parse_stmt(p);
        p->loop_depth--;
        AstStmt* s = ast_new_stmt(p, AST_STMT_WHILE, t);
        s->v.while_stmt.cond = cond;
        s->v.while_stmt.body = body;
        return s;
    }

    if (p->tok.kind == TOK_KW_BREAK) {
        Token t = p->tok;
        parser_next(p);
        if (p->loop_depth <= 0) {
            scc_fatal_at(p->file, p->src, t.line, t.col, "break not within loop");
        }
        parser_expect(p, TOK_SEMI, "Expected ';' after break");
        return ast_new_stmt(p, AST_STMT_BREAK, t);
    }

    if (p->tok.kind == TOK_KW_CONTINUE) {
        Token t = p->tok;
        parser_next(p);
        if (p->loop_depth <= 0) {
            scc_fatal_at(p->file, p->src, t.line, t.col, "continue not within loop");
        }
        parser_expect(p, TOK_SEMI, "Expected ';' after continue");
        return ast_new_stmt(p, AST_STMT_CONTINUE, t);
    }

    if (p->tok.kind == TOK_KW_INT || p->tok.kind == TOK_KW_CHAR || p->tok.kind == TOK_KW_BOOL || p->tok.kind == TOK_KW_VOID || p->tok.kind == TOK_KW_CONST) {
        Token t = p->tok;
        AstStmt* s = ast_new_stmt(p, AST_STMT_DECL, t);
        Type* ty = parse_type(p);
        if (ty->kind == TYPE_VOID) {
            scc_fatal_at(p->file, p->src, t.line, t.col, "Void local variables are not allowed");
        }

        if (p->tok.kind != TOK_IDENT) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected identifier in declaration");
        }

        s->v.decl.decl_name = arena_strndup(p->arena, p->tok.begin, p->tok.len);
        s->v.decl.decl_type = ty;
        parser_next(p);

        Var* dv = scope_add_local(p, s->v.decl.decl_name, ty);
        s->v.decl.decl_var = dv;
        s->v.decl.init = 0;
        if (parser_match(p, TOK_ASSIGN)) {
            s->v.decl.init = parse_expr(p);
        }
        parser_expect(p, TOK_SEMI, "Expected ';' after declaration");
        return s;
    }

    if (p->tok.kind == TOK_KW_RETURN) {
        Token t = p->tok;
        AstStmt* s = ast_new_stmt(p, AST_STMT_RETURN, t);
        parser_next(p);

        if (parser_match(p, TOK_SEMI)) {
            s->v.expr.expr = 0;
            return s;
        }

        s->v.expr.expr = parse_expr(p);
        parser_expect(p, TOK_SEMI, "Expected ';' after return");
        return s;
    }

    if (parser_match(p, TOK_SEMI)) {
        Token t = p->tok;
        AstStmt* s = ast_new_stmt(p, AST_STMT_EXPR, t);
        s->v.expr.expr = 0;
        return s;
    }

    {
        Token t = p->tok;
        AstStmt* s = ast_new_stmt(p, AST_STMT_EXPR, t);
        s->v.expr.expr = parse_expr(p);
        parser_expect(p, TOK_SEMI, "Expected ';' after expression");
        return s;
    }
}

typedef struct {
    Type* ty;
    char* name;
} ParamDecl;

typedef struct {
    ParamDecl* data;
    int count;
} ParamList;

static ParamList parse_param_list(Parser* p) {
    ParamList pl;
    memset(&pl, 0, sizeof(pl));

    FuncType ft;
    memset(&ft, 0, sizeof(ft));

    parser_expect(p, TOK_LPAREN, "Expected '(' after function name");

    ParamDecl tmp_params[16];
    int pc = 0;

    if (p->tok.kind == TOK_RPAREN) {
        parser_next(p);
        pl.data = 0;
        pl.count = 0;
        return pl;
    }

    if (p->tok.kind == TOK_KW_VOID) {
        Token t = p->tok;
        parser_next(p);
        if (p->tok.kind != TOK_RPAREN) {
            scc_fatal_at(p->file, p->src, t.line, t.col, "'void' parameter list must be empty");
        }
        parser_next(p);
        (void)t;
        pl.data = 0;
        pl.count = 0;
        return pl;
    }

    while (1) {
        if (pc >= (int)(sizeof(tmp_params) / sizeof(tmp_params[0]))) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Too many parameters");
        }
        Type* pt = parse_type(p);
        char* pname = 0;
        if (p->tok.kind == TOK_IDENT) {
            pname = arena_strndup(p->arena, p->tok.begin, p->tok.len);
            parser_next(p);
        }
        tmp_params[pc].ty = pt;
        tmp_params[pc].name = pname;
        pc++;
        if (!parser_match(p, TOK_COMMA)) break;
    }

    parser_expect(p, TOK_RPAREN, "Expected ')' after parameter list");

    if (pc) {
        pl.data = (ParamDecl*)arena_alloc(p->arena, (uint32_t)pc * (uint32_t)sizeof(ParamDecl), 8);
        for (int i = 0; i < pc; i++) pl.data[i] = tmp_params[i];
    } else {
        pl.data = 0;
    }
    pl.count = pc;
    return pl;
}

typedef enum {
    TOPLEVEL_NONE = 0,
    TOPLEVEL_FUNC,
    TOPLEVEL_GLOBAL,
} ToplevelKind;

static ToplevelKind parse_toplevel_decl(Parser* p, AstFunc** out_func, AstGlobal** out_global) {
    *out_func = 0;
    *out_global = 0;

    int is_extern = 0;
    if (parser_match(p, TOK_KW_EXTERN)) is_extern = 1;

    Type* first_ty = parse_type(p);

    if (p->tok.kind != TOK_IDENT) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected identifier");
    }

    char* name = arena_strndup(p->arena, p->tok.begin, p->tok.len);
    parser_next(p);

    if (p->tok.kind != TOK_LPAREN) {
        if (first_ty->kind == TYPE_VOID) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Void global variables are not allowed");
        }

        Symbol* sym = symtab_find(p->syms, name);
        if (!sym) {
            sym = symtab_add_global_data(p->syms, p->arena, name, first_ty);
        } else {
            if (sym->kind != SYM_DATA) {
                scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Symbol redeclared with different kind");
            }
        }

        sym->ty = first_ty;
        sym->size = type_size(first_ty);

        AstExpr* init = 0;
        if (parser_match(p, TOK_ASSIGN)) {
            if (is_extern) {
                scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Extern global cannot have an initializer");
            }
            init = parse_expr(p);
        }

        parser_expect(p, TOK_SEMI, "Expected ';' after global declaration");

        AstGlobal* g = (AstGlobal*)arena_alloc(p->arena, sizeof(AstGlobal), 8);
        memset(g, 0, sizeof(*g));
        g->name = name;
        g->ty = first_ty;
        g->init = init;
        g->sym = sym;
        g->next = 0;

        if (is_extern) {
            if (sym->shndx == 0) sym->shndx = SHN_UNDEF;
        } else {
            if (sym->shndx != SHN_UNDEF && sym->shndx != 0) {
                scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Global redefinition");
            }
            sym->shndx = 2;
        }

        *out_global = g;
        return TOPLEVEL_GLOBAL;
    }

    ParamList pl = parse_param_list(p);

    FuncType ft;
    memset(&ft, 0, sizeof(ft));
    ft.ret = first_ty;
    ft.param_count = pl.count;
    if (pl.count) {
        ft.params = (Type**)arena_alloc(p->arena, (uint32_t)pl.count * (uint32_t)sizeof(Type*), 8);
        for (int i = 0; i < pl.count; i++) ft.params[i] = pl.data[i].ty;
    }

    Symbol* sym = symtab_find(p->syms, name);
    if (!sym) {
        sym = symtab_add_func(p->syms, p->arena, name, ft);
    } else {
        if (sym->kind != SYM_FUNC) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Symbol redeclared with different kind");
        }
    }

    if (parser_match(p, TOK_SEMI)) {
        (void)is_extern;
        return TOPLEVEL_NONE;
    }

    if (is_extern) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Extern function cannot have a body");
    }

    if (sym->shndx != SHN_UNDEF) {
        scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Function redefinition");
    }
    sym->shndx = 1;

    parser_expect(p, TOK_LBRACE, "Expected '{' to start function body");

    Var* prev_scope = p->scope_vars;
    ScopeFrame* prev_frames = p->scope_frames;
    int prev_local_size = p->scope_local_size;
    int prev_param_count = p->scope_param_count;

    p->scope_vars = 0;
    p->scope_frames = 0;
    p->scope_local_size = 0;
    p->scope_param_count = pl.count;

    scope_enter(p);

    for (int i = 0; i < pl.count; i++) {
        if (pl.data[i].name) scope_add_param(p, pl.data[i].name, pl.data[i].ty, i);
    }

    AstStmt* first = 0;
    AstStmt* last = 0;
    while (p->tok.kind != TOK_RBRACE) {
        if (p->tok.kind == TOK_EOF) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Unexpected end of file in function body");
        }
        AstStmt* s = parse_stmt(p);
        if (!first) first = s;
        else last->next = s;
        last = s;
    }

    parser_expect(p, TOK_RBRACE, "Expected '}' after function body");

    Var* func_vars = p->scope_vars;
    scope_leave(p);

    AstFunc* f = (AstFunc*)arena_alloc(p->arena, sizeof(AstFunc), 8);
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->first_stmt = first;
    f->sym = sym;
    f->vars = func_vars;
    f->local_size = p->scope_local_size;
    f->param_count = pl.count;
    f->next = 0;

    p->scope_vars = prev_scope;
    p->scope_frames = prev_frames;
    p->scope_local_size = prev_local_size;
    p->scope_param_count = prev_param_count;

    *out_func = f;
    return TOPLEVEL_FUNC;
}

static AstUnit* parse_unit(Parser* p) {
    AstUnit* u = (AstUnit*)arena_alloc(p->arena, sizeof(AstUnit), 8);
    memset(u, 0, sizeof(*u));

    AstFunc* first = 0;
    AstFunc* last = 0;

    AstGlobal* gfirst = 0;
    AstGlobal* glast = 0;

    while (p->tok.kind != TOK_EOF) {
        if (p->tok.kind != TOK_KW_EXTERN && p->tok.kind != TOK_KW_INT && p->tok.kind != TOK_KW_CHAR && p->tok.kind != TOK_KW_BOOL && p->tok.kind != TOK_KW_VOID && p->tok.kind != TOK_KW_CONST) {
            scc_fatal_at(p->file, p->src, p->tok.line, p->tok.col, "Expected top-level declaration");
        }

        AstFunc* f = 0;
        AstGlobal* g = 0;
        ToplevelKind k = parse_toplevel_decl(p, &f, &g);
        if (k == TOPLEVEL_FUNC) {
            if (!first) first = f;
            else last->next = f;
            last = f;
        } else if (k == TOPLEVEL_GLOBAL) {
            if (!gfirst) gfirst = g;
            else glast->next = g;
            glast = g;
        }
    }

    u->first_func = first;
    u->first_global = gfirst;
    return u;
}

#endif

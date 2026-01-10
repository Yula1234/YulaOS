// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_AST_H_INCLUDED
#define SCC_AST_H_INCLUDED

#include "scc_lexer.h"
#include "scc_core.h"

typedef enum {
    AST_EXPR_INT_LIT = 1,
    AST_EXPR_NAME,
    AST_EXPR_STR,
    AST_EXPR_CAST,
    AST_EXPR_UNARY,
    AST_EXPR_BINARY,
    AST_EXPR_ASSIGN,
    AST_EXPR_CALL,
} AstExprKind;

typedef enum {
    AST_UNOP_POS = 1,
    AST_UNOP_NEG,
    AST_UNOP_NOT,
    AST_UNOP_BNOT,
    AST_UNOP_ADDR,
    AST_UNOP_DEREF,
} AstUnOp;

typedef enum {
    AST_BINOP_ADD = 1,
    AST_BINOP_SUB,
    AST_BINOP_MUL,
    AST_BINOP_DIV,
    AST_BINOP_MOD,

    AST_BINOP_SHL,
    AST_BINOP_SHR,

    AST_BINOP_BAND,
    AST_BINOP_BXOR,
    AST_BINOP_BOR,

    AST_BINOP_EQ,
    AST_BINOP_NE,
    AST_BINOP_LT,
    AST_BINOP_LE,
    AST_BINOP_GT,
    AST_BINOP_GE,
    AST_BINOP_ANDAND,
    AST_BINOP_OROR,
} AstBinOp;

typedef struct AstExpr {
    AstExprKind kind;
    Token tok;
    union {
        int32_t int_lit;
        struct {
            char* name;
            Var* var;
            Symbol* sym;
        } name;
        struct {
            char* bytes;
            int len;
        } str;
        struct {
            Type* ty;
            struct AstExpr* expr;
        } cast;
        struct {
            char* callee;
            struct AstExpr** args;
            int arg_count;
        } call;
        struct {
            AstUnOp op;
            struct AstExpr* expr;
        } unary;
        struct {
            AstBinOp op;
            struct AstExpr* left;
            struct AstExpr* right;
        } binary;
        struct {
            struct AstExpr* left;
            struct AstExpr* right;
            AstBinOp op;
        } assign;
    } v;
} AstExpr;

typedef enum {
    AST_STMT_RETURN = 1,
    AST_STMT_EXPR,
    AST_STMT_DECL,
    AST_STMT_BLOCK,
    AST_STMT_IF,
    AST_STMT_WHILE,
    AST_STMT_BREAK,
    AST_STMT_CONTINUE,
} AstStmtKind;

typedef struct AstStmt AstStmt;

struct AstStmt {
    AstStmtKind kind;
    Token tok;
    union {
        struct {
            AstExpr* expr;
        } expr;
        struct {
            Type* decl_type;
            char* decl_name;
            Var* decl_var;
            AstExpr* init;
        } decl;
        struct {
            struct AstStmt* first;
        } block;
        struct {
            AstExpr* cond;
            struct AstStmt* then_stmt;
            struct AstStmt* else_stmt;
        } if_stmt;
        struct {
            AstExpr* cond;
            struct AstStmt* body;
        } while_stmt;
    } v;
    struct AstStmt* next;
};

typedef struct AstFunc {
    char* name;
    AstStmt* first_stmt;
    Symbol* sym;
    Var* vars;
    int local_size;
    int param_count;
    struct AstFunc* next;
} AstFunc;

typedef struct AstGlobal {
    char* name;
    Type* ty;
    AstExpr* init;
    Symbol* sym;
    struct AstGlobal* next;
} AstGlobal;

typedef struct {
    AstFunc* first_func;
    AstGlobal* first_global;
} AstUnit;

#endif

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef SCC_LEXER_H_INCLUDED
#define SCC_LEXER_H_INCLUDED

#include "scc_diag.h"

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUM,
    TOK_STR,

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_SEMI,

    TOK_COMMA,

    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,

    TOK_ASSIGN,

    TOK_PLUSEQ,
    TOK_MINUSEQ,
    TOK_STAREQ,
    TOK_SLASHEQ,
    TOK_PERCENTEQ,

    TOK_AMPEQ,
    TOK_PIPEEQ,
    TOK_CARETEQ,
    TOK_LSHIFTEQ,
    TOK_RSHIFTEQ,

    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,

    TOK_ANDAND,
    TOK_OROR,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,

    TOK_LSHIFT,
    TOK_RSHIFT,

    TOK_BANG,

    TOK_KW_INT,
    TOK_KW_SHORT,
    TOK_KW_LONG,
    TOK_KW_SIGNED,
    TOK_KW_UNSIGNED,
    TOK_KW_CHAR,
    TOK_KW_BOOL,
    TOK_KW_CONST,
    TOK_KW_VOID,
    TOK_KW_RETURN,
    TOK_KW_EXTERN,

    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
} TokenKind;

typedef struct {
    TokenKind kind;
    const char* begin;
    int len;
    int line;
    int col;
    int32_t num_i32;
} Token;

typedef struct {
    const char* file;
    const char* src;
    uint32_t pos;
    int line;
    int col;
} Lexer;

Token lx_next(Lexer* lx);

#endif

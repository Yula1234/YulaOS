#ifndef SCC_LEXER_H_INCLUDED
#define SCC_LEXER_H_INCLUDED

#include "scc_diag.h"

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

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

    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,

    TOK_ANDAND,
    TOK_OROR,
    TOK_AMP,

    TOK_BANG,

    TOK_KW_INT,
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

static char lx_peek(Lexer* lx, uint32_t off) {
    return lx->src[lx->pos + off];
}

static char lx_cur(Lexer* lx) {
    return lx->src[lx->pos];
}

static void lx_advance(Lexer* lx) {
    char c = lx_cur(lx);
    if (c == 0) return;
    lx->pos++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
}

static void lx_skip_ws_and_comments(Lexer* lx) {
    while (1) {
        char c = lx_cur(lx);
        if (is_space(c)) {
            lx_advance(lx);
            continue;
        }

        if (c == '/' && lx_peek(lx, 1) == '/') {
            while (lx_cur(lx) && lx_cur(lx) != '\n') lx_advance(lx);
            continue;
        }

        if (c == '/' && lx_peek(lx, 1) == '*') {
            lx_advance(lx);
            lx_advance(lx);
            while (lx_cur(lx)) {
                if (lx_cur(lx) == '*' && lx_peek(lx, 1) == '/') {
                    lx_advance(lx);
                    lx_advance(lx);
                    break;
                }
                lx_advance(lx);
            }
            continue;
        }

        break;
    }
}

static int tok_text_eq(const Token* t, const char* s) {
    int n = 0;
    while (s[n]) n++;
    if (t->len != n) return 0;
    return memcmp(t->begin, s, (size_t)n) == 0;
}

static Token lx_next(Lexer* lx) {
    lx_skip_ws_and_comments(lx);

    Token t;
    memset(&t, 0, sizeof(t));
    t.begin = &lx->src[lx->pos];
    t.line = lx->line;
    t.col = lx->col;

    char c = lx_cur(lx);
    if (c == 0) {
        t.kind = TOK_EOF;
        t.len = 0;
        return t;
    }

    if (is_alpha(c)) {
        uint32_t start = lx->pos;
        while (is_alnum(lx_cur(lx))) lx_advance(lx);
        t.begin = &lx->src[start];
        t.len = (int)(lx->pos - start);
        t.kind = TOK_IDENT;

        if (tok_text_eq(&t, "int")) t.kind = TOK_KW_INT;
        else if (tok_text_eq(&t, "char")) t.kind = TOK_KW_CHAR;
        else if (tok_text_eq(&t, "bool") || tok_text_eq(&t, "_Bool")) t.kind = TOK_KW_BOOL;
        else if (tok_text_eq(&t, "const")) t.kind = TOK_KW_CONST;
        else if (tok_text_eq(&t, "void")) t.kind = TOK_KW_VOID;
        else if (tok_text_eq(&t, "return")) t.kind = TOK_KW_RETURN;
        else if (tok_text_eq(&t, "extern")) t.kind = TOK_KW_EXTERN;
        else if (tok_text_eq(&t, "if")) t.kind = TOK_KW_IF;
        else if (tok_text_eq(&t, "else")) t.kind = TOK_KW_ELSE;
        else if (tok_text_eq(&t, "while")) t.kind = TOK_KW_WHILE;
        else if (tok_text_eq(&t, "break")) t.kind = TOK_KW_BREAK;
        else if (tok_text_eq(&t, "continue")) t.kind = TOK_KW_CONTINUE;

        return t;
    }

    if (is_digit(c)) {
        uint32_t start = lx->pos;
        int32_t v = 0;
        while (is_digit(lx_cur(lx))) {
            int d = lx_cur(lx) - '0';
            int32_t nv = (int32_t)(v * 10 + d);
            v = nv;
            lx_advance(lx);
        }
        t.begin = &lx->src[start];
        t.len = (int)(lx->pos - start);
        t.kind = TOK_NUM;
        t.num_i32 = v;
        return t;
    }

    if (c == '"') {
        lx_advance(lx);
        uint32_t start = lx->pos;
        while (lx_cur(lx)) {
            char ch = lx_cur(lx);
            if (ch == '"') break;
            if (ch == '\n') scc_fatal_at(lx->file, lx->src, t.line, t.col, "Unterminated string literal");
            if (ch == '\\') {
                lx_advance(lx);
                if (!lx_cur(lx)) break;
                lx_advance(lx);
                continue;
            }
            lx_advance(lx);
        }

        if (lx_cur(lx) != '"') {
            scc_fatal_at(lx->file, lx->src, t.line, t.col, "Unterminated string literal");
        }

        t.begin = &lx->src[start];
        t.len = (int)(lx->pos - start);
        t.kind = TOK_STR;
        lx_advance(lx);
        return t;
    }

    lx_advance(lx);
    t.len = 1;

    if (c == '(') t.kind = TOK_LPAREN;
    else if (c == ')') t.kind = TOK_RPAREN;
    else if (c == '{') t.kind = TOK_LBRACE;
    else if (c == '}') t.kind = TOK_RBRACE;
    else if (c == ';') t.kind = TOK_SEMI;
    else if (c == ',') t.kind = TOK_COMMA;
    else if (c == '+') t.kind = TOK_PLUS;
    else if (c == '-') t.kind = TOK_MINUS;
    else if (c == '*') t.kind = TOK_STAR;
    else if (c == '%') t.kind = TOK_PERCENT;
    else if (c == '/') t.kind = TOK_SLASH;
    else if (c == '=') {
        if (lx_cur(lx) == '=') {
            lx_advance(lx);
            t.len = 2;
            t.kind = TOK_EQ;
        } else {
            t.kind = TOK_ASSIGN;
        }
    } else if (c == '!') {
        if (lx_cur(lx) == '=') {
            lx_advance(lx);
            t.len = 2;
            t.kind = TOK_NE;
        } else {
            t.kind = TOK_BANG;
        }
    } else if (c == '<') {
        if (lx_cur(lx) == '=') {
            lx_advance(lx);
            t.len = 2;
            t.kind = TOK_LE;
        } else {
            t.kind = TOK_LT;
        }
    } else if (c == '>') {
        if (lx_cur(lx) == '=') {
            lx_advance(lx);
            t.len = 2;
            t.kind = TOK_GE;
        } else {
            t.kind = TOK_GT;
        }
    } else if (c == '&') {
        if (lx_cur(lx) == '&') {
            lx_advance(lx);
            t.len = 2;
            t.kind = TOK_ANDAND;
        } else {
            t.kind = TOK_AMP;
        }
    } else if (c == '|') {
        if (lx_cur(lx) == '|') {
            lx_advance(lx);
            t.len = 2;
            t.kind = TOK_OROR;
        } else {
            scc_fatal_at(lx->file, lx->src, t.line, t.col, "Unexpected character");
        }
    } else {
        scc_fatal_at(lx->file, lx->src, t.line, t.col, "Unexpected character");
    }

    return t;
}

#endif

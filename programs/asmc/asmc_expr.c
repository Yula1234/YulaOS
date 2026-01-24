// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_expr.h"

typedef struct {
    const char* s;
    AssemblerCtx* ctx;
} ExprState;

static void expr_skip_spaces(ExprState* st) {
    while (*st->s == ' ' || *st->s == '\t') st->s++;
}

static int expr_parse_number(ExprState* st) {
    expr_skip_spaces(st);
    const char* p = st->s;

    if (p[0] == '0' && p[1] == 'x') {
        p += 2;
        uint32_t val = 0;
        while (*p) {
            char c = *p;
            if (c >= '0' && c <= '9') val = (val << 4) | (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') val = (val << 4) | (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val = (val << 4) | (uint32_t)(c - 'A' + 10);
            else break;
            p++;
        }
        st->s = p;
        return (int)val;
    }

    if (*p >= '0' && *p <= '9') {
        int val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        st->s = p;
        return val;
    }

    return 0;
}

static int expr_parse_identifier(ExprState* st) {
    expr_skip_spaces(st);
    const char* p = st->s;
    char name[64];
    int n = 0;

    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9') || *p == '_' || *p == '.') {
        if (n < 63) {
            name[n++] = *p;
        }
        p++;
    }

    if (n == 0) {
        return 0;
    }

    name[n] = 0;
    st->s = p;

    char full[64];
    resolve_symbol_name(st->ctx, name, full, sizeof(full));
    Symbol* sym = sym_find(st->ctx, full);
    if (sym && sym->section == SEC_ABS) {
        return (int)sym->value;
    }
    return 0;
}

static int expr_parse_or(ExprState* st);

static int expr_parse_primary(ExprState* st) {
    expr_skip_spaces(st);
    if (*st->s == '(') {
        st->s++;
        int v = expr_parse_or(st);

        expr_skip_spaces(st);
        if (*st->s == ')') {
            st->s++;
        }
        return v;
    }

    if ((*st->s >= '0' && *st->s <= '9') || (st->s[0] == '0' && st->s[1] == 'x')) {
        return expr_parse_number(st);
    }

    return expr_parse_identifier(st);
}

static int expr_parse_unary(ExprState* st) {
    expr_skip_spaces(st);
    if (*st->s == '+' || *st->s == '-') {
        char op = *st->s;
        st->s++;
        int v = expr_parse_unary(st);
        return (op == '-') ? -v : v;
    }
    return expr_parse_primary(st);
}

static int expr_parse_mul(ExprState* st) {
    int v = expr_parse_unary(st);
    while (1) {
        expr_skip_spaces(st);
        if (*st->s == '*' || *st->s == '/') {
            char op = *st->s;
            st->s++;
            int rhs = expr_parse_unary(st);
            if (op == '*') v = v * rhs;
            else if (rhs != 0) v = v / rhs;
        } else {
            break;
        }
    }
    return v;
}

static int expr_parse_shift(ExprState* st) {
    int v = expr_parse_mul(st);
    while (1) {
        expr_skip_spaces(st);
        if (st->s[0] == '<' && st->s[1] == '<') {
            st->s += 2;
            int rhs = expr_parse_mul(st);
            v = v << rhs;
        } else if (st->s[0] == '>' && st->s[1] == '>') {
            st->s += 2;
            int rhs = expr_parse_mul(st);
            v = v >> rhs;
        } else {
            break;
        }
    }
    return v;
}

static int expr_parse_add(ExprState* st) {
    int v = expr_parse_shift(st);
    while (1) {
        expr_skip_spaces(st);
        if (*st->s == '+' || *st->s == '-') {
            char op = *st->s;
            st->s++;
            int rhs = expr_parse_shift(st);
            if (op == '+') v = v + rhs;
            else v = v - rhs;
        } else {
            break;
        }
    }
    return v;
}

static int expr_parse_and(ExprState* st) {
    int v = expr_parse_add(st);
    while (1) {
        expr_skip_spaces(st);
        if (*st->s == '&') {
            st->s++;
            int rhs = expr_parse_add(st);
            v = v & rhs;
        } else {
            break;
        }
    }
    return v;
}

static int expr_parse_xor(ExprState* st) {
    int v = expr_parse_and(st);
    while (1) {
        expr_skip_spaces(st);
        if (*st->s == '^') {
            st->s++;
            int rhs = expr_parse_and(st);
            v = v ^ rhs;
        } else {
            break;
        }
    }
    return v;
}

static int expr_parse_or(ExprState* st) {
    int v = expr_parse_xor(st);
    while (1) {
        expr_skip_spaces(st);
        if (*st->s == '|') {
            st->s++;
            int rhs = expr_parse_xor(st);
            v = v | rhs;
        } else {
            break;
        }
    }
    return v;
}

static int eval_simple_number(AssemblerCtx* ctx, const char* s) {
    if (!s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }

    if (s[0] == '0' && s[1] == 'x') {
        s += 2;
        uint32_t val = 0;
        while (*s) {
            val <<= 4;
            if (*s >= '0' && *s <= '9') val |= (*s - '0');
            else if (*s >= 'a' && *s <= 'f') val |= (*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') val |= (*s - 'A' + 10);
            else break;
            s++;
        }
        return (int)val * sign;
    }

    if (*s >= '0' && *s <= '9') {
        int val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        return val * sign;
    }

    char full[64];
    resolve_symbol_name(ctx, s, full, sizeof(full));

    Symbol* sym = sym_find(ctx, full);
    if (sym && sym->section == SEC_ABS) {
        return sym->value * sign;
    }

    return 0;
}

int eval_number(AssemblerCtx* ctx, const char* s) {
    if (!s) return 0;

    const char* p = s;
    int has_ops = 0;
    while (*p) {
        char c = *p;
        if (c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '(' || c == ')' || c == '&' || c == '|' ||
            c == '<' || c == '>' || c == '^') {
            has_ops = 1;
            break;
        }
        p++;
    }

    if (!has_ops) {
        return eval_simple_number(ctx, s);
    }

    ExprState st;
    st.s = s;
    st.ctx = ctx;

    int v = expr_parse_or(&st);
    expr_skip_spaces(&st);
    return v;
}

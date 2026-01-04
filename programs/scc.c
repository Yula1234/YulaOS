// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

// SCC - Small C Compiler

#include <yula.h>

#define SCC_MAX_TOKEN_TEXT 64

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    Elf32_Word    sh_name;
    Elf32_Word    sh_type;
    Elf32_Word    sh_flags;
    Elf32_Addr    sh_addr;
    Elf32_Off     sh_offset;
    Elf32_Word    sh_size;
    Elf32_Word    sh_link;
    Elf32_Word    sh_info;
    Elf32_Word    sh_addralign;
    Elf32_Word    sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    Elf32_Addr    r_offset;
    Elf32_Word    r_info;
} __attribute__((packed)) Elf32_Rel;

#define ET_REL 1
#define EM_386 3

#define R_386_32 1
#define R_386_PC32 2

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_REL 9

#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4

#define STB_LOCAL 0
#define STB_GLOBAL 1

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2

#define SHN_UNDEF 0

#define ELF32_ST_INFO(b,t) (((b)<<4)+((t)&0xf))
#define ELF32_R_INFO(sym,type) (((Elf32_Word)(sym) << 8) | ((Elf32_Word)(type) & 0xFF))

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t cap;
} Buffer;

static void buf_push_u8(Buffer* b, uint8_t v);
static void buf_push_u32(Buffer* b, uint32_t v);

static void buf_init(Buffer* b, uint32_t cap) {
    if (cap == 0) cap = 64;
    b->data = (uint8_t*)malloc(cap);
    if (!b->data) exit(1);
    b->size = 0;
    b->cap = cap;
}

static void emit_x86_sub_esp_imm32(Buffer* text, uint32_t imm) {
    if (imm <= 0x7F) {
        buf_push_u8(text, 0x83);
        buf_push_u8(text, 0xEC);
        buf_push_u8(text, (uint8_t)imm);
        return;
    }
    buf_push_u8(text, 0x81);
    buf_push_u8(text, 0xEC);
    buf_push_u32(text, imm);
}

static void emit_x86_pop_eax(Buffer* text) {
    buf_push_u8(text, 0x58);
}

static void emit_x86_pop_ebx(Buffer* text) {
    buf_push_u8(text, 0x5B);
}

static void emit_x86_pop_edx(Buffer* text) {
    buf_push_u8(text, 0x5A);
}

static void emit_x86_int80(Buffer* text) {
    buf_push_u8(text, 0xCD);
    buf_push_u8(text, 0x80);
}

static void emit_x86_mov_eax_membp_disp(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x8B);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x8B);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_mov_membp_disp_eax(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x89);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_movzx_eax_membp_disp(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x0F);
        buf_push_u8(text, 0xB6);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void emit_x86_mov_membp_disp_al(Buffer* text, int32_t disp) {
    if (disp >= -128 && disp <= 127) {
        buf_push_u8(text, 0x88);
        buf_push_u8(text, 0x45);
        buf_push_u8(text, (uint8_t)(disp & 0xFF));
        return;
    }
    buf_push_u8(text, 0x88);
    buf_push_u8(text, 0x85);
    buf_push_u32(text, (uint32_t)disp);
}

static void buf_free(Buffer* b) {
    if (b->data) free(b->data);
    b->data = 0;
    b->size = 0;
    b->cap = 0;
}

static void buf_reserve(Buffer* b, uint32_t extra) {
    uint32_t need = b->size + extra;
    if (need <= b->cap) return;

    uint32_t ncap = b->cap;
    while (ncap < need) ncap *= 2;

    uint8_t* nd = (uint8_t*)malloc(ncap);
    if (!nd) exit(1);
    if (b->size) memcpy(nd, b->data, b->size);
    free(b->data);
    b->data = nd;
    b->cap = ncap;
}

static void buf_push_u8(Buffer* b, uint8_t v) {
    buf_reserve(b, 1);
    b->data[b->size++] = v;
}

static void buf_push_u32(Buffer* b, uint32_t v) {
    buf_reserve(b, 4);
    b->data[b->size++] = (uint8_t)(v & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->size++] = (uint8_t)((v >> 24) & 0xFF);
}

static void buf_write(Buffer* b, const void* src, uint32_t len) {
    buf_reserve(b, len);
    memcpy(b->data + b->size, src, len);
    b->size += len;
}

static uint32_t buf_add_cstr(Buffer* b, const char* s) {
    uint32_t off = b->size;
    while (*s) buf_push_u8(b, (uint8_t)*s++);
    buf_push_u8(b, 0);
    return off;
}

static void scc_fatal_at(const char* file, const char* src, int line, int col, const char* msg) {
    set_console_color(0xF44747, 0x141414);
    printf("\n[SCC ERROR] %s:%d:%d: %s\n", file ? file : "<input>", line, col, msg ? msg : "error");

    if (src) {
        const char* p = src;
        int cur = 1;
        while (*p && cur < line) {
            if (*p == '\n') cur++;
            p++;
        }

        const char* ls = p;
        while (*p && *p != '\n') p++;
        const char* le = p;

        printf("%.*s\n", (int)(le - ls), ls);
        for (int i = 1; i < col; i++) putchar(' ');
        printf("^\n");
    }

    set_console_color(0xD4D4D4, 0x141414);
    exit(1);
}

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

    TOK_BANG,

    TOK_KW_INT,
    TOK_KW_CHAR,
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
            scc_fatal_at(lx->file, lx->src, t.line, t.col, "Unexpected character");
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

 typedef struct {
     uint8_t* base;
     uint32_t used;
     uint32_t cap;
 } Arena;
 
 static void arena_init(Arena* a, uint32_t cap) {
     if (cap < 4096) cap = 4096;
     a->base = (uint8_t*)malloc(cap);
     if (!a->base) exit(1);
     a->used = 0;
     a->cap = cap;
 }
 
 static void arena_free(Arena* a) {
     if (a->base) free(a->base);
     a->base = 0;
     a->used = 0;
     a->cap = 0;
 }
 
 static void* arena_alloc(Arena* a, uint32_t size, uint32_t align) {
     if (align == 0) align = 1;
     uint32_t p = a->used;
     uint32_t mask = align - 1;
     if ((p & mask) != 0) p = (p + mask) & ~mask;
 
     uint32_t need = p + size;
     if (need > a->cap) {
         uint32_t ncap = a->cap;
         while (ncap < need) ncap *= 2;
         uint8_t* nb = (uint8_t*)malloc(ncap);
         if (!nb) exit(1);
         if (a->used) memcpy(nb, a->base, a->used);
         free(a->base);
         a->base = nb;
         a->cap = ncap;
     }
 
     void* out = a->base + p;
     a->used = need;
     memset(out, 0, size);
     return out;
 }
 
 static char* arena_strndup(Arena* a, const char* s, int len) {
     char* out = (char*)arena_alloc(a, (uint32_t)len + 1, 1);
     memcpy(out, s, (size_t)len);
     out[len] = 0;
     return out;
 }
 
 typedef enum {
     TYPE_INT = 1,
     TYPE_CHAR,
     TYPE_VOID,
     TYPE_PTR,
 } TypeKind;
 
 typedef struct Type {
     TypeKind kind;
     struct Type* base;
     uint8_t is_const;
 } Type;
 
 static uint32_t type_size(Type* ty) {
     if (!ty) return 4;
     if (ty->kind == TYPE_CHAR) return 1;
     if (ty->kind == TYPE_INT) return 4;
     if (ty->kind == TYPE_PTR) return 4;
     return 0;
 }
 
 static uint32_t align_up_u32(uint32_t v, uint32_t align) {
     if (align == 0) return v;
     uint32_t mask = align - 1u;
     return (v + mask) & ~mask;
 }
 
 typedef struct {
     Type* ret;
     Type** params;
     int param_count;
 } FuncType;
 
 typedef enum {
     SYM_FUNC = 1,
     SYM_DATA,
 } SymbolKind;
 
 typedef struct {
     char* name;
     SymbolKind kind;
     Type* ty;
     uint8_t bind;
     uint16_t shndx;
     uint32_t value;
     uint32_t size;
     int elf_index;
     FuncType ftype;
 } Symbol;
 
 typedef struct {
     Symbol* data;
     int count;
     int cap;
 } SymTable;
 
 static void symtab_init(SymTable* st) {
     st->count = 0;
     st->cap = 32;
     st->data = (Symbol*)malloc((uint32_t)st->cap * sizeof(Symbol));
     if (!st->data) exit(1);
     memset(st->data, 0, (uint32_t)st->cap * sizeof(Symbol));
 }
 
 static void symtab_free(SymTable* st) {
     if (st->data) free(st->data);
     st->data = 0;
     st->count = 0;
     st->cap = 0;
 }
 
 static Symbol* symtab_find(SymTable* st, const char* name) {
     for (int i = 0; i < st->count; i++) {
         if (strcmp(st->data[i].name, name) == 0) return &st->data[i];
     }
     return 0;
 }
 
 static Symbol* symtab_add_func(SymTable* st, Arena* a, const char* name, FuncType ft) {
     if (st->count == st->cap) {
         int ncap = st->cap * 2;
         Symbol* nd = (Symbol*)malloc((uint32_t)ncap * sizeof(Symbol));
         if (!nd) exit(1);
         memcpy(nd, st->data, (uint32_t)st->count * sizeof(Symbol));
         memset(nd + st->count, 0, (uint32_t)(ncap - st->count) * sizeof(Symbol));
         free(st->data);
         st->data = nd;
         st->cap = ncap;
     }
 
     Symbol* s = &st->data[st->count++];
     memset(s, 0, sizeof(*s));
     s->name = arena_strndup(a, name, (int)strlen(name));
     s->kind = SYM_FUNC;
     s->ty = 0;
     s->bind = STB_GLOBAL;
     s->shndx = SHN_UNDEF;
     s->value = 0;
     s->size = 0;
     s->elf_index = st->count;
     s->ftype = ft;
     return s;
 }
 
 static Symbol* symtab_add_global_data(SymTable* st, Arena* a, const char* name, Type* ty) {
     if (st->count == st->cap) {
         int ncap = st->cap * 2;
         Symbol* nd = (Symbol*)malloc((uint32_t)ncap * sizeof(Symbol));
         if (!nd) exit(1);
         memcpy(nd, st->data, (uint32_t)st->count * sizeof(Symbol));
         memset(nd + st->count, 0, (uint32_t)(ncap - st->count) * sizeof(Symbol));
         free(st->data);
         st->data = nd;
         st->cap = ncap;
     }
 
     Symbol* s = &st->data[st->count++];
     memset(s, 0, sizeof(*s));
     s->name = arena_strndup(a, name, (int)strlen(name));
     s->kind = SYM_DATA;
     s->ty = ty;
     s->bind = STB_GLOBAL;
     s->shndx = SHN_UNDEF;
     s->value = 0;
     s->size = 4;
     s->elf_index = st->count;
     return s;
 }
 
 static Symbol* symtab_add_local_data(SymTable* st, Arena* a, const char* name, uint32_t value, uint32_t size) {
     if (st->count == st->cap) {
         int ncap = st->cap * 2;
         Symbol* nd = (Symbol*)malloc((uint32_t)ncap * sizeof(Symbol));
         if (!nd) exit(1);
         memcpy(nd, st->data, (uint32_t)st->count * sizeof(Symbol));
         memset(nd + st->count, 0, (uint32_t)(ncap - st->count) * sizeof(Symbol));
         free(st->data);
         st->data = nd;
         st->cap = ncap;
     }
 
     Symbol* s = &st->data[st->count++];
     memset(s, 0, sizeof(*s));
     s->name = arena_strndup(a, name, (int)strlen(name));
     s->kind = SYM_DATA;
     s->ty = 0;
     s->bind = STB_LOCAL;
     s->shndx = 2;
     s->value = value;
     s->size = size;
     s->elf_index = st->count;
     return s;
 }
 
 typedef enum {
     VAR_PARAM = 1,
     VAR_LOCAL,
 } VarKind;
 
 typedef struct Var {
     char* name;
     Type* ty;
     VarKind kind;
     int32_t ebp_offset;
     struct Var* next;
 } Var;
 
 typedef struct ScopeFrame {
     Var* prev_vars;
     struct ScopeFrame* next;
 } ScopeFrame;
 
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
} AstUnOp;

typedef enum {
    AST_BINOP_ADD = 1,
    AST_BINOP_SUB,
    AST_BINOP_MUL,
    AST_BINOP_DIV,
    AST_BINOP_MOD,
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
        if (p->tok.kind == TOK_KW_CONST || p->tok.kind == TOK_KW_INT || p->tok.kind == TOK_KW_CHAR || p->tok.kind == TOK_KW_VOID) {
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
            if (lhs->kind != AST_EXPR_NAME) {
                scc_fatal_at(p->file, p->src, t.line, t.col, "Left-hand side of assignment must be an identifier");
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

    if (p->tok.kind == TOK_KW_INT || p->tok.kind == TOK_KW_CHAR || p->tok.kind == TOK_KW_CONST) {
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
        if (p->tok.kind != TOK_KW_EXTERN && p->tok.kind != TOK_KW_INT && p->tok.kind != TOK_KW_CHAR && p->tok.kind != TOK_KW_VOID && p->tok.kind != TOK_KW_CONST) {
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

static void emit_x86_prologue(Buffer* text) {
    buf_push_u8(text, 0x55);
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xE5);
}

static void emit_x86_mov_eax_imm32(Buffer* text, uint32_t imm) {
    buf_push_u8(text, 0xB8);
    buf_push_u32(text, imm);
}

static void emit_x86_mov_eax_memabs_u32(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0xA1);
    buf_push_u32(text, addr);
}

static void emit_x86_mov_memabs_u32_eax(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0xA3);
    buf_push_u32(text, addr);
}

static void emit_x86_movzx_eax_memabs_u8(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xB6);
    buf_push_u8(text, 0x05);
    buf_push_u32(text, addr);
}

static void emit_x86_mov_memabs_u8_al(Buffer* text, uint32_t addr) {
    buf_push_u8(text, 0x88);
    buf_push_u8(text, 0x05);
    buf_push_u32(text, addr);
}

static void emit_x86_epilogue(Buffer* text);

static void emit_x86_push_eax(Buffer* text) {
    buf_push_u8(text, 0x50);
}

static void emit_x86_pop_ecx(Buffer* text) {
    buf_push_u8(text, 0x59);
}

static void emit_x86_mov_eax_ecx(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xC8);
}

static void emit_x86_mov_ebx_eax(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xC3);
}

static void emit_x86_mov_eax_edx(Buffer* text) {
    buf_push_u8(text, 0x89);
    buf_push_u8(text, 0xD0);
}

static void emit_x86_add_eax_ecx(Buffer* text) {
    buf_push_u8(text, 0x01);
    buf_push_u8(text, 0xC8);
}

static void emit_x86_sub_ecx_eax(Buffer* text) {
    buf_push_u8(text, 0x29);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_imul_eax_ecx(Buffer* text) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, 0xAF);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_cdq(Buffer* text) {
    buf_push_u8(text, 0x99);
}

static void emit_x86_idiv_ebx(Buffer* text) {
    buf_push_u8(text, 0xF7);
    buf_push_u8(text, 0xFB);
}

static void emit_x86_test_eax_eax(Buffer* text) {
    buf_push_u8(text, 0x85);
    buf_push_u8(text, 0xC0);
}

static void emit_x86_cmp_ecx_eax(Buffer* text) {
    buf_push_u8(text, 0x39);
    buf_push_u8(text, 0xC1);
}

static void emit_x86_xor_eax_eax(Buffer* text) {
    buf_push_u8(text, 0x31);
    buf_push_u8(text, 0xC0);
}

static void emit_x86_setcc_al(Buffer* text, uint8_t cc) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, (uint8_t)(0x90u + cc));
    buf_push_u8(text, 0xC0);
}

static uint32_t emit_x86_jcc_rel32_fixup(Buffer* text, uint8_t cc) {
    buf_push_u8(text, 0x0F);
    buf_push_u8(text, (uint8_t)(0x80u + cc));
    uint32_t imm_off = text->size;
    buf_push_u32(text, 0);
    return imm_off;
}

static uint32_t emit_x86_jmp_rel32_fixup(Buffer* text) {
    buf_push_u8(text, 0xE9);
    uint32_t imm_off = text->size;
    buf_push_u32(text, 0);
    return imm_off;
}

static void patch_rel32(Buffer* text, uint32_t imm_off, uint32_t target_off) {
    int32_t rel = (int32_t)target_off - (int32_t)(imm_off + 4u);
    text->data[imm_off + 0] = (uint8_t)(rel & 0xFF);
    text->data[imm_off + 1] = (uint8_t)((rel >> 8) & 0xFF);
    text->data[imm_off + 2] = (uint8_t)((rel >> 16) & 0xFF);
    text->data[imm_off + 3] = (uint8_t)((rel >> 24) & 0xFF);
}

static void emit_x86_neg_eax(Buffer* text) {
    buf_push_u8(text, 0xF7);
    buf_push_u8(text, 0xD8);
}

static void emit_x86_and_eax_imm32(Buffer* text, uint32_t imm) {
    buf_push_u8(text, 0x25);
    buf_push_u32(text, imm);
}

static void emit_x86_call_rel32(Buffer* text, int32_t rel32) {
    buf_push_u8(text, 0xE8);
    buf_push_u32(text, (uint32_t)rel32);
}

static void emit_x86_add_esp_imm32(Buffer* text, uint32_t imm) {
    if (imm <= 0x7F) {
        buf_push_u8(text, 0x83);
        buf_push_u8(text, 0xC4);
        buf_push_u8(text, (uint8_t)imm);
        return;
    }
    buf_push_u8(text, 0x81);
    buf_push_u8(text, 0xC4);
    buf_push_u32(text, imm);
}

 typedef struct {
     uint32_t start_off;
     uint32_t break_fixups[64];
     int break_count;
 } LoopCtx;

typedef struct {
    Buffer* text;
    Buffer* rel_text;
    SymTable* syms;
    Parser* p;

    Buffer* data;
    Buffer* rel_data;

    Var* vars;
    uint32_t str_id;

     LoopCtx loops[16];
     int loop_depth;
} Codegen;

static void gen_expr(Codegen* cg, AstExpr* e);

static void emit_reloc_text(Codegen* cg, uint32_t offset, int sym_index, int type) {
    Elf32_Rel r;
    r.r_offset = (Elf32_Addr)offset;
    r.r_info = ELF32_R_INFO((Elf32_Word)sym_index, (Elf32_Word)type);
    buf_write(cg->rel_text, &r, sizeof(r));
}

static void emit_reloc_data(Codegen* cg, uint32_t offset, int sym_index, int type) {
    Elf32_Rel r;
    r.r_offset = (Elf32_Addr)offset;
    r.r_info = ELF32_R_INFO((Elf32_Word)sym_index, (Elf32_Word)type);
    buf_write(cg->rel_data, &r, sizeof(r));
}

static Var* cg_find_var(Codegen* cg, const char* name) {
    for (Var* v = cg->vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return 0;
}

static void u32_to_dec(char* out, uint32_t v) {
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

static Symbol* cg_intern_string(Codegen* cg, const char* bytes, int len) {
    char namebuf[32];
    char dec[16];
    u32_to_dec(dec, cg->str_id++);

    int n = 0;
    namebuf[n++] = '.';
    namebuf[n++] = 'L';
    namebuf[n++] = 's';
    namebuf[n++] = 't';
    namebuf[n++] = 'r';
    for (int i = 0; dec[i]; i++) namebuf[n++] = dec[i];
    namebuf[n] = 0;

    uint32_t off = cg->data->size;
    if (len) buf_write(cg->data, bytes, (uint32_t)len);
    buf_push_u8(cg->data, 0);

    return symtab_add_local_data(cg->syms, cg->p->arena, namebuf, off, (uint32_t)len + 1u);
}

static void cg_eval_const_u32(Codegen* cg, AstExpr* e, uint32_t* out_val, Symbol** out_reloc_sym) {
    *out_val = 0;
    *out_reloc_sym = 0;

    if (!e) return;

    if (e->kind == AST_EXPR_INT_LIT) {
        *out_val = (uint32_t)e->v.int_lit;
        return;
    }

    if (e->kind == AST_EXPR_STR) {
        Symbol* s = cg_intern_string(cg, e->v.str.bytes, e->v.str.len);
        *out_val = 0;
        *out_reloc_sym = s;
        return;
    }

    if (e->kind == AST_EXPR_CAST) {
        uint32_t v = 0;
        Symbol* rs = 0;
        cg_eval_const_u32(cg, e->v.cast.expr, &v, &rs);

        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_VOID) {
            *out_val = 0;
            *out_reloc_sym = 0;
            return;
        }

        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_CHAR) {
            if (rs) {
                scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Cannot cast relocatable address to char in global initializer");
            }
            v &= 0xFFu;
        }

        *out_val = v;
        *out_reloc_sym = rs;
        return;
    }

    if (e->kind == AST_EXPR_UNARY) {
        uint32_t v = 0;
        Symbol* rs = 0;
        cg_eval_const_u32(cg, e->v.unary.expr, &v, &rs);
        if (rs) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Relocatable address is not supported in unary global initializer");
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
        cg_eval_const_u32(cg, e->v.binary.left, &lv, &ls);
        cg_eval_const_u32(cg, e->v.binary.right, &rv, &rs);
        if (ls || rs) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Relocatable address is not supported in binary global initializer");
        }

        if (e->v.binary.op == AST_BINOP_ADD) { *out_val = lv + rv; return; }
        if (e->v.binary.op == AST_BINOP_SUB) { *out_val = lv - rv; return; }
        if (e->v.binary.op == AST_BINOP_MUL) { *out_val = lv * rv; return; }
        if (e->v.binary.op == AST_BINOP_DIV) {
            if (rv == 0) scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Division by zero in global initializer");
            *out_val = (uint32_t)((int32_t)lv / (int32_t)rv);
            return;
        }
        if (e->v.binary.op == AST_BINOP_MOD) {
            if (rv == 0) scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Modulo by zero in global initializer");
            *out_val = (uint32_t)((int32_t)lv % (int32_t)rv);
            return;
        }

        scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Only arithmetic operators are supported in global initializers");
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Non-constant global initializer");
}

static void gen_expr_binary_arith(Codegen* cg, AstExpr* e) {
    gen_expr(cg, e->v.binary.left);
    emit_x86_push_eax(cg->text);
    gen_expr(cg, e->v.binary.right);
    emit_x86_pop_ecx(cg->text);

    if (e->v.binary.op == AST_BINOP_ADD) {
        emit_x86_add_eax_ecx(cg->text);
        return;
    }

    if (e->v.binary.op == AST_BINOP_SUB) {
        emit_x86_sub_ecx_eax(cg->text);
        emit_x86_mov_eax_ecx(cg->text);
        return;
    }

    if (e->v.binary.op == AST_BINOP_MUL) {
        emit_x86_imul_eax_ecx(cg->text);
        return;
    }

    if (e->v.binary.op == AST_BINOP_DIV || e->v.binary.op == AST_BINOP_MOD) {
        emit_x86_mov_ebx_eax(cg->text);
        emit_x86_mov_eax_ecx(cg->text);
        emit_x86_cdq(cg->text);
        emit_x86_idiv_ebx(cg->text);
        if (e->v.binary.op == AST_BINOP_MOD) {
            emit_x86_mov_eax_edx(cg->text);
        }
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Binary operator not supported in codegen yet");
}

static void gen_expr_binary_cmp(Codegen* cg, AstExpr* e) {
    gen_expr(cg, e->v.binary.left);
    emit_x86_push_eax(cg->text);
    gen_expr(cg, e->v.binary.right);
    emit_x86_pop_ecx(cg->text);

    emit_x86_cmp_ecx_eax(cg->text);
    emit_x86_mov_eax_imm32(cg->text, 0);

    if (e->v.binary.op == AST_BINOP_EQ) {
        emit_x86_setcc_al(cg->text, 0x4);
        return;
    }
    if (e->v.binary.op == AST_BINOP_NE) {
        emit_x86_setcc_al(cg->text, 0x5);
        return;
    }
    if (e->v.binary.op == AST_BINOP_LT) {
        emit_x86_setcc_al(cg->text, 0xC);
        return;
    }
    if (e->v.binary.op == AST_BINOP_LE) {
        emit_x86_setcc_al(cg->text, 0xE);
        return;
    }
    if (e->v.binary.op == AST_BINOP_GT) {
        emit_x86_setcc_al(cg->text, 0xF);
        return;
    }
    if (e->v.binary.op == AST_BINOP_GE) {
        emit_x86_setcc_al(cg->text, 0xD);
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Comparison operator not supported in codegen yet");
}

static void gen_expr_binary_logical(Codegen* cg, AstExpr* e) {
    if (e->v.binary.op == AST_BINOP_ANDAND) {
        gen_expr(cg, e->v.binary.left);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jz_false = emit_x86_jcc_rel32_fixup(cg->text, 0x4);

        gen_expr(cg, e->v.binary.right);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jz_false2 = emit_x86_jcc_rel32_fixup(cg->text, 0x4);

        emit_x86_mov_eax_imm32(cg->text, 1);
        uint32_t jmp_end = emit_x86_jmp_rel32_fixup(cg->text);

        uint32_t false_off = cg->text->size;
        emit_x86_mov_eax_imm32(cg->text, 0);
        uint32_t end_off = cg->text->size;

        patch_rel32(cg->text, jz_false, false_off);
        patch_rel32(cg->text, jz_false2, false_off);
        patch_rel32(cg->text, jmp_end, end_off);
        return;
    }

    if (e->v.binary.op == AST_BINOP_OROR) {
        gen_expr(cg, e->v.binary.left);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jnz_true = emit_x86_jcc_rel32_fixup(cg->text, 0x5);

        gen_expr(cg, e->v.binary.right);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jnz_true2 = emit_x86_jcc_rel32_fixup(cg->text, 0x5);

        emit_x86_mov_eax_imm32(cg->text, 0);
        uint32_t jmp_end = emit_x86_jmp_rel32_fixup(cg->text);

        uint32_t true_off = cg->text->size;
        emit_x86_mov_eax_imm32(cg->text, 1);
        uint32_t end_off = cg->text->size;

        patch_rel32(cg->text, jnz_true, true_off);
        patch_rel32(cg->text, jnz_true2, true_off);
        patch_rel32(cg->text, jmp_end, end_off);
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Logical operator not supported in codegen yet");
}

static void gen_expr(Codegen* cg, AstExpr* e) {
    if (!e) {
        emit_x86_mov_eax_imm32(cg->text, 0);
        return;
    }

    if (e->kind == AST_EXPR_INT_LIT) {
        emit_x86_mov_eax_imm32(cg->text, (uint32_t)e->v.int_lit);
        return;
    }

    if (e->kind == AST_EXPR_NAME) {
        Var* v = e->v.name.var;
        if (v) {
            if (v->ty && v->ty->kind == TYPE_CHAR) {
                emit_x86_movzx_eax_membp_disp(cg->text, v->ebp_offset);
            } else {
                emit_x86_mov_eax_membp_disp(cg->text, v->ebp_offset);
            }
            return;
        }

        Symbol* s = e->v.name.sym;
        if (!s) s = symtab_find(cg->syms, e->v.name.name);
        if (!s || s->kind != SYM_DATA) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Unknown identifier");
        }

        uint32_t off = cg->text->size;
        if (s->ty && s->ty->kind == TYPE_CHAR) {
            emit_x86_movzx_eax_memabs_u8(cg->text, 0);
            emit_reloc_text(cg, off + 3, s->elf_index, R_386_32);
        } else {
            emit_x86_mov_eax_memabs_u32(cg->text, 0);
            emit_reloc_text(cg, off + 1, s->elf_index, R_386_32);
        }
        return;
    }

    if (e->kind == AST_EXPR_STR) {
        Symbol* s = cg_intern_string(cg, e->v.str.bytes, e->v.str.len);
        uint32_t off = cg->text->size;
        emit_x86_mov_eax_imm32(cg->text, 0);
        emit_reloc_text(cg, off + 1, s->elf_index, R_386_32);
        return;
    }

    if (e->kind == AST_EXPR_CAST) {
        gen_expr(cg, e->v.cast.expr);
        if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_CHAR) {
            emit_x86_and_eax_imm32(cg->text, 0xFF);
        } else if (e->v.cast.ty && e->v.cast.ty->kind == TYPE_VOID) {
            emit_x86_mov_eax_imm32(cg->text, 0);
        }
        return;
    }

    if (e->kind == AST_EXPR_CALL) {
        if (strcmp(e->v.call.callee, "__syscall") == 0) {
            if (e->v.call.arg_count != 4) {
                scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "__syscall requires exactly 4 arguments");
            }

            for (int i = 0; i < 4; i++) {
                gen_expr(cg, e->v.call.args[i]);
                emit_x86_push_eax(cg->text);
            }

            emit_x86_pop_edx(cg->text);
            emit_x86_pop_ecx(cg->text);
            emit_x86_pop_ebx(cg->text);
            emit_x86_pop_eax(cg->text);
            emit_x86_int80(cg->text);
            return;
        }

        Symbol* s = symtab_find(cg->syms, e->v.call.callee);
        if (!s || s->kind != SYM_FUNC) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Call to undeclared function");
        }

        if (s->ftype.param_count != e->v.call.arg_count) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Argument count mismatch in call");
        }

        for (int i = e->v.call.arg_count - 1; i >= 0; i--) {
            gen_expr(cg, e->v.call.args[i]);
            emit_x86_push_eax(cg->text);
        }

        uint32_t call_site = cg->text->size;
        emit_x86_call_rel32(cg->text, -4);
        emit_reloc_text(cg, call_site + 1, s->elf_index, R_386_PC32);

        uint32_t stack_bytes = (uint32_t)e->v.call.arg_count * 4u;
        if (stack_bytes) emit_x86_add_esp_imm32(cg->text, stack_bytes);
        return;
    }

    if (e->kind == AST_EXPR_UNARY) {
        gen_expr(cg, e->v.unary.expr);
        if (e->v.unary.op == AST_UNOP_NEG) {
            emit_x86_neg_eax(cg->text);
        } else if (e->v.unary.op == AST_UNOP_NOT) {
            emit_x86_test_eax_eax(cg->text);
            emit_x86_mov_eax_imm32(cg->text, 0);
            emit_x86_setcc_al(cg->text, 0x4);
        }
        return;
    }

    if (e->kind == AST_EXPR_BINARY) {
        if (e->v.binary.op == AST_BINOP_ADD || e->v.binary.op == AST_BINOP_SUB || e->v.binary.op == AST_BINOP_MUL || e->v.binary.op == AST_BINOP_DIV || e->v.binary.op == AST_BINOP_MOD) {
            gen_expr_binary_arith(cg, e);
        } else if (e->v.binary.op == AST_BINOP_EQ || e->v.binary.op == AST_BINOP_NE || e->v.binary.op == AST_BINOP_LT || e->v.binary.op == AST_BINOP_LE || e->v.binary.op == AST_BINOP_GT || e->v.binary.op == AST_BINOP_GE) {
            gen_expr_binary_cmp(cg, e);
        } else if (e->v.binary.op == AST_BINOP_ANDAND || e->v.binary.op == AST_BINOP_OROR) {
            gen_expr_binary_logical(cg, e);
        } else {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Binary operator not supported in codegen yet");
        }
        return;
    }

    if (e->kind == AST_EXPR_ASSIGN) {
        if (e->v.assign.left->kind != AST_EXPR_NAME) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Invalid assignment target");
        }

        gen_expr(cg, e->v.assign.right);

        Var* v = e->v.assign.left->v.name.var;
        if (v) {
            if (v->ty && v->ty->kind == TYPE_CHAR) {
                emit_x86_mov_membp_disp_al(cg->text, v->ebp_offset);
            } else {
                emit_x86_mov_membp_disp_eax(cg->text, v->ebp_offset);
            }
            return;
        }

        Symbol* s = e->v.assign.left->v.name.sym;
        if (!s) s = symtab_find(cg->syms, e->v.assign.left->v.name.name);
        if (!s || s->kind != SYM_DATA) {
            scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Unknown identifier in assignment");
        }

        uint32_t off = cg->text->size;
        if (s->ty && s->ty->kind == TYPE_CHAR) {
            emit_x86_mov_memabs_u8_al(cg->text, 0);
            emit_reloc_text(cg, off + 2, s->elf_index, R_386_32);
        } else {
            emit_x86_mov_memabs_u32_eax(cg->text, 0);
            emit_reloc_text(cg, off + 1, s->elf_index, R_386_32);
        }
        return;
    }

    scc_fatal_at(cg->p->file, cg->p->src, e->tok.line, e->tok.col, "Unknown expression kind");
}

static int gen_stmt(Codegen* cg, AstStmt* s);

static int gen_stmt_list(Codegen* cg, AstStmt* s) {
    while (s) {
        int did = gen_stmt(cg, s);
        if (did) return 1;
        s = s->next;
    }
    return 0;
}

static int gen_stmt(Codegen* cg, AstStmt* s) {
    if (!s) return 0;

    if (s->kind == AST_STMT_DECL) {
        if (s->v.decl.init) {
            gen_expr(cg, s->v.decl.init);
            Var* v = s->v.decl.decl_var;
            if (!v) scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Internal error: decl var not found");
            if (v->ty && v->ty->kind == TYPE_CHAR) emit_x86_mov_membp_disp_al(cg->text, v->ebp_offset);
            else emit_x86_mov_membp_disp_eax(cg->text, v->ebp_offset);
        }
        return 0;
    }

    if (s->kind == AST_STMT_EXPR) {
        if (s->v.expr.expr) gen_expr(cg, s->v.expr.expr);
        return 0;
    }

    if (s->kind == AST_STMT_RETURN) {
        gen_expr(cg, s->v.expr.expr);
        emit_x86_epilogue(cg->text);
        return 1;
    }

    if (s->kind == AST_STMT_BLOCK) {
        return gen_stmt_list(cg, s->v.block.first);
    }

    if (s->kind == AST_STMT_IF) {
        gen_expr(cg, s->v.if_stmt.cond);
        emit_x86_test_eax_eax(cg->text);

        if (s->v.if_stmt.else_stmt) {
            uint32_t jz_else = emit_x86_jcc_rel32_fixup(cg->text, 0x4);
            int then_ret = gen_stmt(cg, s->v.if_stmt.then_stmt);
            uint32_t jmp_end = emit_x86_jmp_rel32_fixup(cg->text);
            uint32_t else_off = cg->text->size;
            patch_rel32(cg->text, jz_else, else_off);
            int else_ret = gen_stmt(cg, s->v.if_stmt.else_stmt);
            uint32_t end_off = cg->text->size;
            patch_rel32(cg->text, jmp_end, end_off);
            return (then_ret && else_ret) ? 1 : 0;
        }

        uint32_t jz_end = emit_x86_jcc_rel32_fixup(cg->text, 0x4);
        (void)gen_stmt(cg, s->v.if_stmt.then_stmt);
        uint32_t end_off = cg->text->size;
        patch_rel32(cg->text, jz_end, end_off);
        return 0;
    }

    if (s->kind == AST_STMT_WHILE) {
        uint32_t start_off = cg->text->size;

        if (cg->loop_depth >= (int)(sizeof(cg->loops) / sizeof(cg->loops[0]))) {
            scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Loop nesting too deep");
        }
        LoopCtx* lc = &cg->loops[cg->loop_depth++];
        memset(lc, 0, sizeof(*lc));
        lc->start_off = start_off;

        gen_expr(cg, s->v.while_stmt.cond);
        emit_x86_test_eax_eax(cg->text);
        uint32_t jz_end = emit_x86_jcc_rel32_fixup(cg->text, 0x4);
        (void)gen_stmt(cg, s->v.while_stmt.body);
        uint32_t jmp_back = emit_x86_jmp_rel32_fixup(cg->text);
        patch_rel32(cg->text, jmp_back, start_off);
        uint32_t end_off = cg->text->size;
        patch_rel32(cg->text, jz_end, end_off);

        for (int i = 0; i < lc->break_count; i++) {
            patch_rel32(cg->text, lc->break_fixups[i], end_off);
        }

        cg->loop_depth--;
        return 0;
    }

    if (s->kind == AST_STMT_BREAK) {
        if (cg->loop_depth <= 0) {
            scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "break not within loop");
        }
        LoopCtx* lc = &cg->loops[cg->loop_depth - 1];
        if (lc->break_count >= (int)(sizeof(lc->break_fixups) / sizeof(lc->break_fixups[0]))) {
            scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Too many breaks in loop");
        }
        uint32_t jmp = emit_x86_jmp_rel32_fixup(cg->text);
        lc->break_fixups[lc->break_count++] = jmp;
        return 0;
    }

    if (s->kind == AST_STMT_CONTINUE) {
        if (cg->loop_depth <= 0) {
            scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "continue not within loop");
        }
        LoopCtx* lc = &cg->loops[cg->loop_depth - 1];
        uint32_t jmp = emit_x86_jmp_rel32_fixup(cg->text);
        patch_rel32(cg->text, jmp, lc->start_off);
        return 0;
    }

    scc_fatal_at(cg->p->file, cg->p->src, s->tok.line, s->tok.col, "Unknown statement kind");
    return 0;
}

static void emit_x86_epilogue(Buffer* text) {
    buf_push_u8(text, 0xC9);
    buf_push_u8(text, 0xC3);
}

static void write_elf_object(const char* out_path, Buffer* text, Buffer* data, uint32_t bss_size, Buffer* rel_text, Buffer* rel_data, SymTable* syms) {
    Buffer strtab; buf_init(&strtab, 128);
    buf_push_u8(&strtab, 0);

    Buffer symtab; buf_init(&symtab, 128);
    Elf32_Sym null_sym;
    memset(&null_sym, 0, sizeof(null_sym));
    buf_write(&symtab, &null_sym, sizeof(null_sym));

    for (int i = 0; i < syms->count; i++) {
        Symbol* ss = &syms->data[i];
        uint32_t name_off = buf_add_cstr(&strtab, ss->name);

        Elf32_Sym es;
        memset(&es, 0, sizeof(es));
        es.st_name = name_off;
        es.st_value = ss->value;
        es.st_size = ss->size;
        unsigned char st_type = (ss->kind == SYM_FUNC) ? STT_FUNC : STT_OBJECT;
        es.st_info = ELF32_ST_INFO(ss->bind, st_type);
        es.st_other = 0;
        es.st_shndx = ss->shndx;
        buf_write(&symtab, &es, sizeof(es));
    }

    Buffer shstr; buf_init(&shstr, 128);
    buf_push_u8(&shstr, 0);

    int n_txt = (int)buf_add_cstr(&shstr, ".text");
    int n_dat = (int)buf_add_cstr(&shstr, ".data");
    int n_bss = (int)buf_add_cstr(&shstr, ".bss");
    int n_sym = (int)buf_add_cstr(&shstr, ".symtab");
    int n_str = (int)buf_add_cstr(&shstr, ".strtab");
    int n_shs = (int)buf_add_cstr(&shstr, ".shstrtab");
    int n_rt  = (int)buf_add_cstr(&shstr, ".rel.text");
    int n_rd  = (int)buf_add_cstr(&shstr, ".rel.data");

    uint32_t offset = sizeof(Elf32_Ehdr);
    uint32_t off_txt = offset; offset += text->size;
    uint32_t off_dat = offset; offset += data->size;
    uint32_t off_bss = offset;
    uint32_t off_sym = offset; offset += symtab.size;
    uint32_t off_str = offset; offset += strtab.size;
    uint32_t off_shs = offset; offset += shstr.size;
    uint32_t off_rt  = offset; offset += rel_text->size;
    uint32_t off_rd  = offset; offset += rel_data->size;
    uint32_t off_shdr = offset;

    Elf32_Ehdr eh;
    memset(&eh, 0, sizeof(eh));
    eh.e_ident[0] = 0x7F;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = 1;
    eh.e_ident[5] = 1;
    eh.e_ident[6] = 1;
    eh.e_type = ET_REL;
    eh.e_machine = EM_386;
    eh.e_version = 1;
    eh.e_ehsize = sizeof(Elf32_Ehdr);
    eh.e_shoff = off_shdr;
    eh.e_shentsize = sizeof(Elf32_Shdr);
    eh.e_shnum = 9;
    eh.e_shstrndx = 6;

    int fd = open(out_path, 1);
    if (fd < 0) {
        printf("Cannot write output: %s\n", out_path);
        exit(1);
    }

    write(fd, &eh, sizeof(eh));
    if (text->size) write(fd, text->data, text->size);
    if (data->size) write(fd, data->data, data->size);
    write(fd, symtab.data, symtab.size);
    write(fd, strtab.data, strtab.size);
    write(fd, shstr.data, shstr.size);
    if (rel_text->size) write(fd, rel_text->data, rel_text->size);
    if (rel_data->size) write(fd, rel_data->data, rel_data->size);

    Elf32_Shdr sh[9];
    memset(sh, 0, sizeof(sh));

    sh[1].sh_name = (Elf32_Word)n_txt;
    sh[1].sh_type = SHT_PROGBITS;
    sh[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    sh[1].sh_offset = off_txt;
    sh[1].sh_size = text->size;
    sh[1].sh_addralign = 4;

    sh[2].sh_name = (Elf32_Word)n_dat;
    sh[2].sh_type = SHT_PROGBITS;
    sh[2].sh_flags = SHF_ALLOC | SHF_WRITE;
    sh[2].sh_offset = off_dat;
    sh[2].sh_size = data->size;
    sh[2].sh_addralign = 4;

    sh[3].sh_name = (Elf32_Word)n_bss;
    sh[3].sh_type = SHT_NOBITS;
    sh[3].sh_flags = SHF_ALLOC | SHF_WRITE;
    sh[3].sh_offset = off_bss;
    sh[3].sh_size = bss_size;
    sh[3].sh_addralign = 4;

    sh[4].sh_name = (Elf32_Word)n_sym;
    sh[4].sh_type = SHT_SYMTAB;
    sh[4].sh_offset = off_sym;
    sh[4].sh_size = symtab.size;
    sh[4].sh_link = 5;
    int local_count = 0;
    for (int i = 0; i < syms->count; i++) {
        if (syms->data[i].bind == STB_LOCAL) local_count++;
    }
    sh[4].sh_info = (Elf32_Word)(1 + local_count);
    sh[4].sh_entsize = sizeof(Elf32_Sym);
    sh[4].sh_addralign = 4;

    sh[5].sh_name = (Elf32_Word)n_str;
    sh[5].sh_type = SHT_STRTAB;
    sh[5].sh_offset = off_str;
    sh[5].sh_size = strtab.size;
    sh[5].sh_addralign = 1;

    sh[6].sh_name = (Elf32_Word)n_shs;
    sh[6].sh_type = SHT_STRTAB;
    sh[6].sh_offset = off_shs;
    sh[6].sh_size = shstr.size;
    sh[6].sh_addralign = 1;

    sh[7].sh_name = (Elf32_Word)n_rt;
    sh[7].sh_type = SHT_REL;
    sh[7].sh_offset = off_rt;
    sh[7].sh_size = rel_text->size;
    sh[7].sh_link = 4;
    sh[7].sh_info = 1;
    sh[7].sh_entsize = sizeof(Elf32_Rel);
    sh[7].sh_addralign = 4;

    sh[8].sh_name = (Elf32_Word)n_rd;
    sh[8].sh_type = SHT_REL;
    sh[8].sh_offset = off_rd;
    sh[8].sh_size = rel_data->size;
    sh[8].sh_link = 4;
    sh[8].sh_info = 2;
    sh[8].sh_entsize = sizeof(Elf32_Rel);
    sh[8].sh_addralign = 4;

    write(fd, sh, sizeof(sh));
    close(fd);

    buf_free(&strtab);
    buf_free(&symtab);
    buf_free(&shstr);
}

static char* read_entire_file(const char* path, Buffer* tmp_storage) {
    int fd = open(path, 0);
    if (fd < 0) {
        printf("Cannot open input file: %s\n", path);
        return 0;
    }

    buf_init(tmp_storage, 4096);

    while (1) {
        char chunk[1024];
        int r = read(fd, chunk, (int)sizeof(chunk));
        if (r < 0) {
            close(fd);
            printf("Read error: %s\n", path);
            return 0;
        }
        if (r == 0) break;
        buf_write(tmp_storage, chunk, (uint32_t)r);
    }

    close(fd);
    buf_push_u8(tmp_storage, 0);
    return (char*)tmp_storage->data;
}

static void scc_compile_file(const char* in_path, const char* out_path) {
    Buffer src_storage;
    char* src = read_entire_file(in_path, &src_storage);
    if (!src) exit(1);

    Arena arena;
    arena_init(&arena, 16 * 1024);

    SymTable syms;
    symtab_init(&syms);

    Parser p;
    memset(&p, 0, sizeof(p));
    p.file = in_path;
    p.src = src;
    p.lx.file = in_path;
    p.lx.src = src;
    p.lx.pos = 0;
    p.lx.line = 1;
    p.lx.col = 1;
    p.arena = &arena;
    p.syms = &syms;

    parser_next(&p);
    AstUnit* u = parse_unit(&p);

    Buffer text;
    buf_init(&text, 64);

    Buffer data;
    buf_init(&data, 64);

    Buffer rel_text;
    buf_init(&rel_text, 64);

    Buffer rel_data;
    buf_init(&rel_data, 16);

    Codegen cg;
    memset(&cg, 0, sizeof(cg));
    cg.text = &text;
    cg.rel_text = &rel_text;
    cg.syms = &syms;
    cg.p = &p;
    cg.data = &data;
    cg.rel_data = &rel_data;
    cg.str_id = 0;

    uint32_t bss_size = 0;
    for (AstGlobal* g = u->first_global; g; g = g->next) {
        if (!g->sym) continue;
        if (g->sym->shndx == SHN_UNDEF) continue;

        uint32_t sz = type_size(g->ty);
        uint32_t al = (sz == 1) ? 1u : 4u;

        if (g->init) {
            uint32_t aligned = align_up_u32(data.size, al);
            while (data.size < aligned) buf_push_u8(&data, 0);

            g->sym->shndx = 2;
            g->sym->value = data.size;
            g->sym->size = sz;

            uint32_t v = 0;
            Symbol* rs = 0;
            cg_eval_const_u32(&cg, g->init, &v, &rs);

            if (sz == 1) {
                if (rs) scc_fatal_at(p.file, p.src, g->init->tok.line, g->init->tok.col, "Relocation is not supported for 1-byte global initializer");
                buf_push_u8(&data, (uint8_t)v);
            } else if (sz == 4) {
                uint32_t off = data.size;
                buf_push_u32(&data, v);
                if (rs) emit_reloc_data(&cg, off, rs->elf_index, R_386_32);
            } else {
                scc_fatal_at(p.file, p.src, g->init->tok.line, g->init->tok.col, "Unsupported global type size");
            }
        } else {
            bss_size = align_up_u32(bss_size, al);
            g->sym->shndx = 3;
            g->sym->value = bss_size;
            g->sym->size = sz;
            bss_size += sz;
        }
    }

    for (AstFunc* f = u->first_func; f; f = f->next) {
        while ((text.size & 3u) != 0) buf_push_u8(&text, 0);

        uint32_t func_start = text.size;
        f->sym->value = func_start;
        cg.vars = f->vars;
        emit_x86_prologue(&text);

        if (f->local_size) {
            emit_x86_sub_esp_imm32(&text, (uint32_t)f->local_size);
        }

        int did_return = gen_stmt_list(&cg, f->first_stmt);
        if (!did_return) {
            emit_x86_mov_eax_imm32(&text, 0);
            emit_x86_epilogue(&text);
        }

        uint32_t func_size = text.size - func_start;
        f->sym->size = func_size;
    }

    write_elf_object(out_path, &text, &data, bss_size, &rel_text, &rel_data, &syms);

    buf_free(&text);
    buf_free(&data);
    buf_free(&rel_text);
    buf_free(&rel_data);
    symtab_free(&syms);
    arena_free(&arena);
    buf_free(&src_storage);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("SCC v0.1\nUsage: scc -o out.o input.c\n       scc input.c out.o\n");
        return 1;
    }

    const char* in_path = 0;
    const char* out_path = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                printf("Missing value after -o\n");
                return 1;
            }
            out_path = argv[++i];
            continue;
        }

        if (!in_path) in_path = argv[i];
        else if (!out_path) out_path = argv[i];
        else {
            printf("Unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!in_path || !out_path) {
        printf("Invalid arguments\n");
        return 1;
    }

    scc_compile_file(in_path, out_path);
    printf("Success: %s\n", out_path);
    return 0;
}

#ifndef SCC_CORE_H_INCLUDED
#define SCC_CORE_H_INCLUDED

#include "scc_elf.h"

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

#endif

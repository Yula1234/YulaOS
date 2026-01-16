#ifndef SCC_PP_H_INCLUDED
#define SCC_PP_H_INCLUDED

#include "scc_common.h"
#include "scc_buffer.h"
#include "scc_diag.h"

typedef struct {
    const char* name;
    const char* value;
} SccPPDefine;

typedef struct {
    const char** include_paths;
    int include_path_count;
    const SccPPDefine* defines;
    int define_count;
    int max_include_depth;
    int allow_extensions;
} SccPPConfig;

typedef struct {
    int ok;
    char* text;
} SccPPResult;

static SccPPResult scc_preprocess_file(const SccPPConfig* cfg, const char* input_path);

typedef struct {
    const char* begin;
    int len;
} SccPPSlice;

typedef struct SccPP SccPP;

static int scc_pp_is_space(char c);
static int scc_pp_is_alpha(char c);
static int scc_pp_is_digit(char c);
static int scc_pp_is_alnum(char c);

static int64_t scc_pp_mul_i64(int64_t a, int64_t b);
static int64_t scc_pp_shl_i64(int64_t v, uint32_t sh);

typedef enum {
    SCC_PP_TOK_EOF = 0,
    SCC_PP_TOK_IDENT,
    SCC_PP_TOK_NUM,
    SCC_PP_TOK_STR,
    SCC_PP_TOK_CHAR,
    SCC_PP_TOK_WS,
    SCC_PP_TOK_NL,
    SCC_PP_TOK_PUNCT,
    SCC_PP_TOK_HASH,
    SCC_PP_TOK_HASHHASH,
} SccPPTokenKind;

typedef struct {
    SccPPTokenKind kind;
    const char* begin;
    int len;
    int line;
    int col;
} SccPPToken;

typedef struct {
    SccPP* pp;
    const char* file;
    const char* src;
    int pos;
    int line;
    int col;
} SccPPScanner;

static void scc_pp_fatal_at(SccPP* pp, const char* file, const char* src, int line, int col, const char* msg);

static char scc_pp_sc_cur(SccPPScanner* sc) {
    return sc->src[sc->pos];
}

static char scc_pp_sc_peek(SccPPScanner* sc, int off) {
    return sc->src[sc->pos + off];
}

static void scc_pp_sc_advance(SccPPScanner* sc) {
    char c = scc_pp_sc_cur(sc);
    if (!c) return;
    sc->pos++;
    if (c == '\r') {
        if (scc_pp_sc_cur(sc) == '\n') sc->pos++;
        sc->line++;
        sc->col = 1;
        return;
    }
    if (c == '\n') {
        sc->line++;
        sc->col = 1;
    } else {
        sc->col++;
    }
}

static SccPPToken scc_pp_tok_make(SccPPTokenKind k, const char* b, int n, int line, int col) {
    SccPPToken t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    t.begin = b;
    t.len = n;
    t.line = line;
    t.col = col;
    return t;
}

static SccPPToken scc_pp_next_token(SccPPScanner* sc) {
    char c = scc_pp_sc_cur(sc);
    if (!c) return scc_pp_tok_make(SCC_PP_TOK_EOF, sc->src + sc->pos, 0, sc->line, sc->col);

    if (c == '\r') {
        int line = sc->line;
        int col = sc->col;
        if (scc_pp_sc_peek(sc, 1) == '\n') {
            sc->pos += 2;
        } else {
            sc->pos += 1;
        }
        sc->line++;
        sc->col = 1;
        return scc_pp_tok_make(SCC_PP_TOK_NL, "\n", 1, line, col);
    }

    if (c == '\n') {
        int line = sc->line;
        int col = sc->col;
        scc_pp_sc_advance(sc);
        return scc_pp_tok_make(SCC_PP_TOK_NL, "\n", 1, line, col);
    }

    if (scc_pp_is_space(c)) {
        int line = sc->line;
        int col = sc->col;
        int start = sc->pos;
        while (1) {
            char ch = scc_pp_sc_cur(sc);
            if (!ch || ch == '\n' || ch == '\r') break;
            if (ch == '\\' && scc_pp_sc_peek(sc, 1) == '\n') break;
            if (!scc_pp_is_space(ch)) break;
            scc_pp_sc_advance(sc);
        }
        return scc_pp_tok_make(SCC_PP_TOK_WS, sc->src + start, sc->pos - start, line, col);
    }

    if (c == '/' && scc_pp_sc_peek(sc, 1) == '/') {
        int line = sc->line;
        int col = sc->col;
        while (scc_pp_sc_cur(sc) && scc_pp_sc_cur(sc) != '\n' && scc_pp_sc_cur(sc) != '\r') scc_pp_sc_advance(sc);
        return scc_pp_tok_make(SCC_PP_TOK_WS, " ", 1, line, col);
    }

    if (c == '/' && scc_pp_sc_peek(sc, 1) == '*') {
        int line = sc->line;
        int col = sc->col;
        int start_line = line;
        int start_col = col;
        scc_pp_sc_advance(sc);
        scc_pp_sc_advance(sc);
        int closed = 0;
        while (scc_pp_sc_cur(sc)) {
            if (scc_pp_sc_cur(sc) == '*' && scc_pp_sc_peek(sc, 1) == '/') {
                scc_pp_sc_advance(sc);
                scc_pp_sc_advance(sc);
                closed = 1;
                break;
            }
            scc_pp_sc_advance(sc);
        }
        if (!closed) {
            scc_pp_fatal_at(sc ? sc->pp : 0, sc ? sc->file : 0, sc ? sc->src : 0, start_line, start_col, "Preprocessor: unterminated /* comment");
        }
        return scc_pp_tok_make(SCC_PP_TOK_WS, " ", 1, line, col);
    }

    if (c == '#') {
        int line = sc->line;
        int col = sc->col;
        if (scc_pp_sc_peek(sc, 1) == '#') {
            scc_pp_sc_advance(sc);
            scc_pp_sc_advance(sc);
            return scc_pp_tok_make(SCC_PP_TOK_HASHHASH, "##", 2, line, col);
        }
        scc_pp_sc_advance(sc);
        return scc_pp_tok_make(SCC_PP_TOK_HASH, "#", 1, line, col);
    }

    if (scc_pp_is_alpha(c)) {
        int line = sc->line;
        int col = sc->col;
        int start = sc->pos;
        while (scc_pp_is_alnum(scc_pp_sc_cur(sc))) scc_pp_sc_advance(sc);
        return scc_pp_tok_make(SCC_PP_TOK_IDENT, sc->src + start, sc->pos - start, line, col);
    }

    if (scc_pp_is_digit(c)) {
        int line = sc->line;
        int col = sc->col;
        int start = sc->pos;
        while (scc_pp_is_alnum(scc_pp_sc_cur(sc)) || scc_pp_sc_cur(sc) == '.') scc_pp_sc_advance(sc);
        return scc_pp_tok_make(SCC_PP_TOK_NUM, sc->src + start, sc->pos - start, line, col);
    }

    if (c == '"') {
        int line = sc->line;
        int col = sc->col;
        int start = sc->pos;
        scc_pp_sc_advance(sc);
        while (scc_pp_sc_cur(sc)) {
            char ch = scc_pp_sc_cur(sc);
            if (ch == '"') {
                scc_pp_sc_advance(sc);
                break;
            }
            if (ch == '\n' || ch == '\r') {
                scc_pp_fatal_at(sc ? sc->pp : 0, sc ? sc->file : 0, sc ? sc->src : 0, line, col, "Preprocessor: unterminated string literal");
            }
            if (ch == '\\') {
                scc_pp_sc_advance(sc);
                if (scc_pp_sc_cur(sc)) scc_pp_sc_advance(sc);
                continue;
            }
            scc_pp_sc_advance(sc);
        }
        if (!scc_pp_sc_cur(sc) && sc->src[sc->pos - 1] != '"') {
            scc_pp_fatal_at(sc ? sc->pp : 0, sc ? sc->file : 0, sc ? sc->src : 0, line, col, "Preprocessor: unterminated string literal");
        }
        return scc_pp_tok_make(SCC_PP_TOK_STR, sc->src + start, sc->pos - start, line, col);
    }

    if (c == '\'') {
        int line = sc->line;
        int col = sc->col;
        int start = sc->pos;
        scc_pp_sc_advance(sc);
        while (scc_pp_sc_cur(sc)) {
            char ch = scc_pp_sc_cur(sc);
            if (ch == '\'') {
                scc_pp_sc_advance(sc);
                break;
            }
            if (ch == '\n' || ch == '\r') {
                scc_pp_fatal_at(sc ? sc->pp : 0, sc ? sc->file : 0, sc ? sc->src : 0, line, col, "Preprocessor: unterminated character literal");
            }
            if (ch == '\\') {
                scc_pp_sc_advance(sc);
                if (scc_pp_sc_cur(sc)) scc_pp_sc_advance(sc);
                continue;
            }
            scc_pp_sc_advance(sc);
        }
        if (!scc_pp_sc_cur(sc) && sc->src[sc->pos - 1] != '\'') {
            scc_pp_fatal_at(sc ? sc->pp : 0, sc ? sc->file : 0, sc ? sc->src : 0, line, col, "Preprocessor: unterminated character literal");
        }
        return scc_pp_tok_make(SCC_PP_TOK_CHAR, sc->src + start, sc->pos - start, line, col);
    }

    {
        int line = sc->line;
        int col = sc->col;
        const char* b = sc->src + sc->pos;
        scc_pp_sc_advance(sc);
        return scc_pp_tok_make(SCC_PP_TOK_PUNCT, b, 1, line, col);
    }
}

typedef struct {
    char* name;
    int is_func;
    int is_variadic;
    int param_count;
    char** params;
    char* repl_src;
    SccPPToken* repl;
    int repl_count;
    int repl_cap;
} SccPPMacro;

typedef struct {
    int parent_active;
    int active;
    int any_true;
    int seen_else;
} SccPPIfFrame;

typedef struct SccPP {
    const SccPPConfig* cfg;

    SccPPMacro* macros;
    int macro_count;
    int macro_cap;

    char** once_files;
    int once_count;
    int once_cap;

    char** include_stack;
    int include_depth;
    int include_cap;

    const char** expanding;
    int expanding_count;
    int expanding_cap;

    SccPPIfFrame* ifs;
    int if_count;
    int if_cap;
    int max_include_depth;
} SccPP;

static int scc_pp_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int scc_pp_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int scc_pp_is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static int scc_pp_is_alnum(char c) {
    return scc_pp_is_alpha(c) || scc_pp_is_digit(c);
}

static char* scc_pp_strndup(const char* s, int n) {
    if (n < 0) n = 0;
    char* out = (char*)malloc((size_t)n + 1);
    if (!out) exit(1);
    if (n) memcpy(out, s, (size_t)n);
    out[n] = 0;
    return out;
}

static void scc_pp_normalize_newlines_inplace(Buffer* b) {
    if (!b || !b->data || b->size == 0) return;
    uint32_t w = 0;
    uint32_t n = b->size;
    for (uint32_t r = 0; r < n; r++) {
        uint8_t c = b->data[r];
        if (c == 0) break;
        if (c == (uint8_t)'\r') {
            if (r + 1 < n && b->data[r + 1] == (uint8_t)'\n') r++;
            b->data[w++] = (uint8_t)'\n';
            continue;
        }
        b->data[w++] = c;
    }
    b->data[w++] = 0;
    b->size = w;
}

static void scc_pp_splice_backslash_newlines_inplace(Buffer* b) {
    if (!b || !b->data || b->size == 0) return;
    uint32_t w = 0;
    uint32_t n = b->size;
    for (uint32_t r = 0; r < n; r++) {
        uint8_t c = b->data[r];
        if (c == (uint8_t)'\\') {
            if (r + 1 < n && b->data[r + 1] == (uint8_t)'\n') {
                r += 1;
                continue;
            }
            if (r + 1 < n && b->data[r + 1] == (uint8_t)'\r') {
                if (r + 2 < n && b->data[r + 2] == (uint8_t)'\n') r += 2;
                else r += 1;
                continue;
            }
        }
        b->data[w++] = b->data[r];
    }
    if (w == 0 || b->data[w - 1] != 0) {
        b->data[w++] = 0;
    }
    b->size = w;
}

static int scc_pp_slice_eq(SccPPSlice a, const char* s) {
    int n = 0;
    while (s[n]) n++;
    if (a.len != n) return 0;
    return memcmp(a.begin, s, (size_t)n) == 0;
}

static int scc_pp_last_slash(const char* path) {
    if (!path) return -1;
    int last = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last = i;
    }
    return last;
}

static char* scc_pp_dirname_dup(const char* path) {
    int i = scc_pp_last_slash(path);
    if (i < 0) return scc_pp_strndup(".", 1);
    if (i == 0) return scc_pp_strndup("/", 1);
    return scc_pp_strndup(path, i);
}

static char* scc_pp_path_join(const char* dir, const char* rel) {
    if (!rel || !rel[0]) return scc_pp_strndup("", 0);
    if (rel[0] == '/') return scc_pp_strndup(rel, (int)strlen(rel));
    if (!dir || !dir[0]) return scc_pp_strndup(rel, (int)strlen(rel));

    int dl = (int)strlen(dir);
    int rl = (int)strlen(rel);
    int need_slash = (dl > 0 && dir[dl - 1] != '/') ? 1 : 0;

    char* out = (char*)malloc((size_t)dl + (size_t)need_slash + (size_t)rl + 1);
    if (!out) exit(1);

    memcpy(out, dir, (size_t)dl);
    int p = dl;
    if (need_slash) out[p++] = '/';
    memcpy(out + p, rel, (size_t)rl);
    out[p + rl] = 0;
    return out;
}

static char* scc_pp_read_entire_file(const char* path, Buffer* tmp_storage) {
    int fd = open(path, 0);
    if (fd < 0) return 0;

    buf_init(tmp_storage, 4096);

    while (1) {
        char chunk[1024];
        int r = read(fd, chunk, (int)sizeof(chunk));
        if (r < 0) {
            close(fd);
            buf_free(tmp_storage);
            return 0;
        }
        if (r == 0) break;
        buf_write(tmp_storage, chunk, (uint32_t)r);
    }

    close(fd);
    buf_push_u8(tmp_storage, 0);
    scc_pp_normalize_newlines_inplace(tmp_storage);
    scc_pp_splice_backslash_newlines_inplace(tmp_storage);
    return (char*)tmp_storage->data;
}

static void scc_pp_init(SccPP* pp, const SccPPConfig* cfg) {
    memset(pp, 0, sizeof(*pp));
    pp->cfg = cfg;
    pp->max_include_depth = cfg && cfg->max_include_depth ? cfg->max_include_depth : 64;
}

static void scc_pp_free(SccPP* pp) {
    if (pp->macros) {
        for (int i = 0; i < pp->macro_count; i++) {
            SccPPMacro* m = &pp->macros[i];
            if (m->name) free(m->name);
            if (m->params) {
                for (int k = 0; k < m->param_count; k++) {
                    if (m->params[k]) free(m->params[k]);
                }
                free(m->params);
            }
            if (m->repl_src) free(m->repl_src);
            if (m->repl) free(m->repl);
        }
        free(pp->macros);
    }
    if (pp->once_files) {
        for (int i = 0; i < pp->once_count; i++) {
            if (pp->once_files[i]) free(pp->once_files[i]);
        }
        free(pp->once_files);
    }
    if (pp->include_stack) {
        for (int i = 0; i < pp->include_depth; i++) {
            if (pp->include_stack[i]) free(pp->include_stack[i]);
        }
        free(pp->include_stack);
    }

    if (pp->expanding) free(pp->expanding);
    if (pp->ifs) free(pp->ifs);
    memset(pp, 0, sizeof(*pp));
}

static int scc_pp_is_active(SccPP* pp) {
    if (!pp || pp->if_count <= 0) return 1;
    return pp->ifs[pp->if_count - 1].active;
}

static void scc_pp_if_push(SccPP* pp, int parent_active, int cond_true) {
    if (pp->if_count == pp->if_cap) {
        int ncap = (pp->if_cap == 0) ? 16 : (pp->if_cap * 2);
        void* nd = realloc(pp->ifs, (size_t)ncap * sizeof(*pp->ifs));
        if (!nd) exit(1);
        pp->ifs = nd;
        pp->if_cap = ncap;
    }
    int act = parent_active && cond_true;
    pp->ifs[pp->if_count++] = (SccPPIfFrame){
        .parent_active = parent_active,
        .active = act,
        .any_true = act,
        .seen_else = 0,
    };
}

static void scc_pp_if_elif(SccPP* pp, const char* file, const char* src, int line, int cond_true) {
    if (!pp || pp->if_count <= 0) {
        scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: #elif without #if");
    }
    SccPPIfFrame* f = &pp->ifs[pp->if_count - 1];
    if (f->seen_else) {
        scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: #elif after #else");
    }
    if (!f->parent_active) {
        f->active = 0;
        return;
    }
    if (f->any_true) {
        f->active = 0;
        return;
    }
    f->active = cond_true ? 1 : 0;
    if (f->active) f->any_true = 1;
}

static void scc_pp_if_else(SccPP* pp, const char* file, const char* src, int line) {
    if (!pp || pp->if_count <= 0) {
        scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: #else without #if");
    }
    SccPPIfFrame* f = &pp->ifs[pp->if_count - 1];
    if (f->seen_else) {
        scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: multiple #else");
    }
    f->seen_else = 1;
    f->active = (f->parent_active && !f->any_true) ? 1 : 0;
    f->any_true = 1;
}

static void scc_pp_if_pop(SccPP* pp, const char* file, const char* src, int line) {
    if (!pp || pp->if_count <= 0) {
        scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: #endif without #if");
    }
    pp->if_count--;
}

static int scc_pp_is_expanding(SccPP* pp, const char* name) {
    for (int i = pp->expanding_count - 1; i >= 0; i--) {
        const char* s = pp->expanding[i];
        if (s && strcmp(s, name) == 0) return 1;
    }
    return 0;
}

static int scc_pp_tok_is_dec_digit_seq(SccPPToken t) {
    if (t.kind != SCC_PP_TOK_NUM || !t.begin || t.len <= 0) return 0;
    for (int i = 0; i < t.len; i++) {
        char c = t.begin[i];
        if (!(c >= '0' && c <= '9')) return 0;
    }
    return 1;
}

static int64_t scc_pp_parse_dec_digit_seq_token(SccPPToken t) {
    int64_t v = 0;
    for (int i = 0; i < t.len; i++) {
        v = scc_pp_mul_i64(v, 10) + (t.begin[i] - '0');
    }
    return v;
}

static void scc_pp_push_expanding(SccPP* pp, const char* name) {
    if (pp->expanding_count == pp->expanding_cap) {
        int ncap = (pp->expanding_cap == 0) ? 16 : (pp->expanding_cap * 2);
        const char** nd = (const char**)realloc(pp->expanding, (size_t)ncap * sizeof(const char*));
        if (!nd) exit(1);
        pp->expanding = nd;
        pp->expanding_cap = ncap;
    }
    pp->expanding[pp->expanding_count++] = name;
}

static void scc_pp_pop_expanding(SccPP* pp) {
    if (pp->expanding_count > 0) pp->expanding_count--;
}

static void scc_pp_emit(Buffer* out, const char* s, int n) {
    if (!s || n <= 0) return;
    buf_write(out, s, (uint32_t)n);
}

static void scc_pp_emit_cstr(Buffer* out, const char* s) {
    if (!s) return;
    buf_write(out, s, (uint32_t)strlen(s));
}

static void scc_pp_include_push(SccPP* pp, const char* path) {
    if (!pp || !path || !path[0]) return;
    if (pp->include_depth == pp->include_cap) {
        int ncap = (pp->include_cap == 0) ? 16 : (pp->include_cap * 2);
        char** nd = (char**)realloc(pp->include_stack, (size_t)ncap * sizeof(char*));
        if (!nd) exit(1);
        pp->include_stack = nd;
        pp->include_cap = ncap;
    }
    pp->include_stack[pp->include_depth++] = scc_pp_strndup(path, (int)strlen(path));
}

static void scc_pp_include_pop(SccPP* pp) {
    if (!pp || pp->include_depth <= 0) return;
    pp->include_depth--;
    if (pp->include_stack && pp->include_stack[pp->include_depth]) {
        free(pp->include_stack[pp->include_depth]);
        pp->include_stack[pp->include_depth] = 0;
    }
}

static void scc_pp_fatal_at(SccPP* pp, const char* file, const char* src, int line, int col, const char* msg) {
    if (pp && pp->include_stack && pp->include_depth > 1) {
        int n = pp->include_depth;
        printf("\n");
        printf("In file included from %s\n", pp->include_stack[0] ? pp->include_stack[0] : "<input>");
        for (int i = 1; i < n - 1; i++) {
            printf("                 from %s\n", pp->include_stack[i] ? pp->include_stack[i] : "<input>");
        }
    }
    scc_fatal_at(file, src, line, col, msg);
}

static void scc_pp_emit_u32_dec(Buffer* out, uint32_t v) {
    char buf[16];
    int p = 0;
    if (v == 0) {
        buf[p++] = '0';
    } else {
        char tmp[16];
        int tp = 0;
        while (v > 0 && tp < (int)sizeof(tmp)) {
            tmp[tp++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (tp > 0) buf[p++] = tmp[--tp];
    }
    scc_pp_emit(out, buf, p);
}

static SccPPMacro* scc_pp_find_macro(SccPP* pp, SccPPSlice name) {
    for (int i = 0; i < pp->macro_count; i++) {
        SccPPMacro* m = &pp->macros[i];
        if (!m->name) continue;
        int n = (int)strlen(m->name);
        if (n != name.len) continue;
        if (memcmp(m->name, name.begin, (size_t)n) == 0) return m;
    }
    return 0;
}

static void scc_pp_undef(SccPP* pp, SccPPSlice name) {
    for (int i = 0; i < pp->macro_count; i++) {
        SccPPMacro* m = &pp->macros[i];
        if (!m->name) continue;
        int n = (int)strlen(m->name);
        if (n != name.len) continue;
        if (memcmp(m->name, name.begin, (size_t)n) != 0) continue;
        if (m->name) free(m->name);
        if (m->params) {
            for (int k = 0; k < m->param_count; k++) {
                if (m->params[k]) free(m->params[k]);
            }
            free(m->params);
        }
        if (m->repl_src) free(m->repl_src);
        if (m->repl) free(m->repl);
        *m = (SccPPMacro){0};
        return;
    }
}

static void scc_pp_macro_set_repl_from_text(SccPPMacro* m, const char* text) {
    if (m->repl_src) free(m->repl_src);
    if (m->repl) free(m->repl);
    m->repl_src = 0;
    m->repl = 0;
    m->repl_count = 0;
    m->repl_cap = 0;

    if (!text) text = "";
    m->repl_src = scc_pp_strndup(text, (int)strlen(text));

    SccPPScanner sc;
    memset(&sc, 0, sizeof(sc));
    sc.pp = 0;
    sc.file = 0;
    sc.src = m->repl_src;
    sc.pos = 0;
    sc.line = 1;
    sc.col = 1;

    int cap = 32;
    int cnt = 0;
    SccPPToken* toks = (SccPPToken*)malloc((size_t)cap * sizeof(SccPPToken));
    if (!toks) exit(1);

    while (1) {
        SccPPToken t = scc_pp_next_token(&sc);
        if (t.kind == SCC_PP_TOK_EOF) break;
        if (t.kind == SCC_PP_TOK_NL) continue;

        if (cnt == cap) {
            cap *= 2;
            SccPPToken* nt = (SccPPToken*)realloc(toks, (size_t)cap * sizeof(SccPPToken));
            if (!nt) exit(1);
            toks = nt;
        }
        toks[cnt++] = t;
    }

    m->repl = toks;
    m->repl_count = cnt;
    m->repl_cap = cap;
}

static int scc_pp_tok_is_punct1(SccPPToken t, char c) {
    return t.kind == SCC_PP_TOK_PUNCT && t.len == 1 && t.begin && t.begin[0] == c;
}

static int scc_pp_tok_is_ellipsis(SccPPToken* toks, int i, int count) {
    if (i + 2 >= count) return 0;
    return scc_pp_tok_is_punct1(toks[i], '.') && scc_pp_tok_is_punct1(toks[i + 1], '.') && scc_pp_tok_is_punct1(toks[i + 2], '.');
}

static int scc_pp_param_index(SccPPMacro* m, SccPPSlice name) {
    for (int i = 0; i < m->param_count; i++) {
        const char* p = m->params[i];
        int n = (int)strlen(p);
        if (n != name.len) continue;
        if (memcmp(p, name.begin, (size_t)n) == 0) return i;
    }
    return -1;
}

static void scc_pp_trim_ws_range(SccPPToken* toks, int* io_start, int* io_end) {
    int s = *io_start;
    int e = *io_end;
    while (s < e && toks[s].kind == SCC_PP_TOK_WS) s++;
    while (e > s && toks[e - 1].kind == SCC_PP_TOK_WS) e--;
    *io_start = s;
    *io_end = e;
}

static char* scc_pp_tokens_to_text_dup(SccPPToken* toks, int start, int end) {
    Buffer b;
    buf_init(&b, 64);
    for (int i = start; i < end; i++) {
        scc_pp_emit(&b, toks[i].begin, toks[i].len);
    }
    buf_push_u8(&b, 0);
    return (char*)b.data;
}

static char* scc_pp_stringize_text_dup(const char* s) {
    if (!s) s = "";
    int n = (int)strlen(s);
    int i = 0;
    while (i < n && scc_pp_is_space(s[i])) i++;
    while (n > i && scc_pp_is_space(s[n - 1])) n--;

    Buffer b;
    buf_init(&b, (uint32_t)(n - i + 16));

    buf_push_u8(&b, (uint8_t)'"');
    int was_ws = 0;
    for (int k = i; k < n; k++) {
        char c = s[k];
        if (scc_pp_is_space(c)) {
            if (!was_ws) {
                buf_push_u8(&b, (uint8_t)' ');
                was_ws = 1;
            }
            continue;
        }
        was_ws = 0;
        if (c == '"' || c == '\\') {
            buf_push_u8(&b, (uint8_t)'\\');
            buf_push_u8(&b, (uint8_t)c);
        } else {
            buf_push_u8(&b, (uint8_t)c);
        }
    }
    buf_push_u8(&b, (uint8_t)'"');
    buf_push_u8(&b, 0);
    return (char*)b.data;
}

static int scc_pp_text_has_ws(const char* s) {
    if (!s) return 0;
    for (int i = 0; s[i]; i++) {
        if (scc_pp_is_space(s[i])) return 1;
    }
    return 0;
}

static void scc_pp_tokenize_text_no_nl(const char* text, SccPPToken** out_toks, int* out_count) {
    SccPPScanner sc;
    memset(&sc, 0, sizeof(sc));
    sc.pp = 0;
    sc.file = 0;
    sc.src = text ? text : "";
    sc.pos = 0;
    sc.line = 1;
    sc.col = 1;

    int cap = 32;
    int cnt = 0;
    SccPPToken* toks = (SccPPToken*)malloc((size_t)cap * sizeof(SccPPToken));
    if (!toks) exit(1);

    while (1) {
        SccPPToken t = scc_pp_next_token(&sc);
        if (t.kind == SCC_PP_TOK_EOF) break;
        if (t.kind == SCC_PP_TOK_NL) continue;
        if (cnt == cap) {
            cap *= 2;
            SccPPToken* nt = (SccPPToken*)realloc(toks, (size_t)cap * sizeof(SccPPToken));
            if (!nt) exit(1);
            toks = nt;
        }
        toks[cnt++] = t;
    }

    *out_toks = toks;
    *out_count = cnt;
}

typedef struct {
    char** raw;
    char** exp;
    int count;
    int cap;
} SccPPArgs;

static void scc_pp_expand_tokens(SccPP* pp, const char* cur_file, const char* cur_src, SccPPToken* toks, int count, Buffer* out, int inv_line, int depth);

typedef struct {
    SccPP* pp;
    SccPPToken* toks;
    int count;
    int pos;
    const char* file;
    const char* src;
    int line;
} SccPPExpr;

static void scc_pp_expr_skip_ws(SccPPExpr* e) {
    while (e->pos < e->count && e->toks[e->pos].kind == SCC_PP_TOK_WS) e->pos++;
}

static int scc_pp_expr_peek_punct(SccPPExpr* e, char c) {
    scc_pp_expr_skip_ws(e);
    if (e->pos >= e->count) return 0;
    return scc_pp_tok_is_punct1(e->toks[e->pos], c);
}

static int scc_pp_expr_match_punct1(SccPPExpr* e, char c) {
    if (!scc_pp_expr_peek_punct(e, c)) return 0;
    e->pos++;
    return 1;
}

static int scc_pp_expr_match_punct2(SccPPExpr* e, char a, char b) {
    scc_pp_expr_skip_ws(e);
    if (e->pos + 1 >= e->count) return 0;
    if (!scc_pp_tok_is_punct1(e->toks[e->pos], a)) return 0;
    if (!scc_pp_tok_is_punct1(e->toks[e->pos + 1], b)) return 0;
    e->pos += 2;
    return 1;
}

static void scc_pp_expr_fail(SccPPExpr* e) {
    scc_pp_fatal_at(e ? e->pp : 0, e ? e->file : 0, e ? e->src : 0, e ? e->line : 1, 1, "Preprocessor: invalid #if expression");
}

static int scc_pp_is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int scc_pp_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

static int64_t scc_pp_parse_int64_token(SccPPToken t) {
    const char* s = t.begin;
    int n = t.len;
    if (!s || n <= 0) return 0;

    int i = 0;
    int base = 10;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (n >= 1 && s[0] == '0') {
        base = 8;
        i = 1;
    }

    int64_t v = 0;
    for (; i < n; i++) {
        char c = s[i];
        int d = -1;
        if (base == 16) {
            if (!scc_pp_is_hex_digit(c)) break;
            d = scc_pp_hex_val(c);
        } else if (base == 10) {
            if (!(c >= '0' && c <= '9')) break;
            d = c - '0';
        } else {
            if (!(c >= '0' && c <= '7')) break;
            d = c - '0';
        }
        v = scc_pp_mul_i64(v, (int64_t)base) + (int64_t)d;
    }
    return v;
}

static int64_t scc_pp_parse_char_token(SccPPToken t) {
    const char* s = t.begin;
    int n = t.len;
    if (!s || n < 2) return 0;
    if (s[0] != '\'' || s[n - 1] != '\'') return 0;
    if (n == 3) return (unsigned char)s[1];
    if (n >= 4 && s[1] == '\\') {
        char c = s[2];
        if (c == 'n') return '\n';
        if (c == 'r') return '\r';
        if (c == 't') return '\t';
        if (c == '0') return 0;
        if (c == '\\') return '\\';
        if (c == '\'') return '\'';
        if (c == '"') return '"';
        if (c == 'x') {
            int64_t v = 0;
            int i = 3;
            while (i < n - 1 && scc_pp_is_hex_digit(s[i])) {
                v = scc_pp_shl_i64(v, 4) | (int64_t)scc_pp_hex_val(s[i]);
                i++;
            }
            return v;
        }
        if (c >= '0' && c <= '7') {
            int64_t v = 0;
            int i = 2;
            int cnt = 0;
            while (i < n - 1 && cnt < 3 && s[i] >= '0' && s[i] <= '7') {
                v = scc_pp_shl_i64(v, 3) | (int64_t)(s[i] - '0');
                i++;
                cnt++;
            }
            return v;
        }
        return (unsigned char)c;
    }
    return (unsigned char)s[1];
}

static int64_t scc_pp_expr_parse_cond(SccPPExpr* e, int eval);

static int64_t scc_pp_expr_parse_primary(SccPPExpr* e, int eval) {
    scc_pp_expr_skip_ws(e);
    if (e->pos >= e->count) scc_pp_expr_fail(e);
    SccPPToken t = e->toks[e->pos];

    if (scc_pp_tok_is_punct1(t, '(')) {
        e->pos++;
        int64_t v = scc_pp_expr_parse_cond(e, eval);
        if (!scc_pp_expr_match_punct1(e, ')')) scc_pp_expr_fail(e);
        return v;
    }

    if (t.kind == SCC_PP_TOK_NUM) {
        e->pos++;
        return eval ? scc_pp_parse_int64_token(t) : 0;
    }
    if (t.kind == SCC_PP_TOK_CHAR) {
        e->pos++;
        return eval ? scc_pp_parse_char_token(t) : 0;
    }
    if (t.kind == SCC_PP_TOK_IDENT) {
        e->pos++;
        return 0;
    }

    scc_pp_expr_fail(e);
    return 0;
}

static int64_t scc_pp_expr_parse_unary(SccPPExpr* e, int eval) {
    if (scc_pp_expr_match_punct1(e, '!')) {
        int64_t v = scc_pp_expr_parse_unary(e, eval);
        return eval ? !v : 0;
    }
    if (scc_pp_expr_match_punct1(e, '~')) {
        int64_t v = scc_pp_expr_parse_unary(e, eval);
        return eval ? ~v : 0;
    }
    if (scc_pp_expr_match_punct1(e, '+')) {
        int64_t v = scc_pp_expr_parse_unary(e, eval);
        return eval ? +v : 0;
    }
    if (scc_pp_expr_match_punct1(e, '-')) {
        int64_t v = scc_pp_expr_parse_unary(e, eval);
        return eval ? -v : 0;
    }
    return scc_pp_expr_parse_primary(e, eval);
}

typedef struct {
    uint32_t lo;
    uint32_t hi;
} SccPPU64Parts;

static SccPPU64Parts scc_pp_u64_parts_from_u64(uint64_t v) {
    SccPPU64Parts p;
    memcpy(&p, &v, sizeof(p));
    return p;
}

static uint64_t scc_pp_u64_from_parts(SccPPU64Parts p) {
    uint64_t v = 0;
    memcpy(&v, &p, sizeof(p));
    return v;
}

static SccPPU64Parts scc_pp_u64_parts_from_i64(int64_t v) {
    uint64_t u = 0;
    memcpy(&u, &v, sizeof(u));
    return scc_pp_u64_parts_from_u64(u);
}

static void scc_pp_u64_shl1(SccPPU64Parts* x) {
    uint32_t carry = (x->lo >> 31) & 1u;
    x->lo <<= 1;
    x->hi = (x->hi << 1) | carry;
}

static void scc_pp_u64_shr1(SccPPU64Parts* x) {
    uint32_t carry = (x->hi & 1u);
    x->hi >>= 1;
    x->lo = (x->lo >> 1) | (carry << 31);
}

static int scc_pp_u64_ge(SccPPU64Parts a, SccPPU64Parts b) {
    if (a.hi != b.hi) return a.hi > b.hi;
    return a.lo >= b.lo;
}

static void scc_pp_u64_sub(SccPPU64Parts* a, SccPPU64Parts b) {
    uint32_t old_lo = a->lo;
    a->lo -= b.lo;
    a->hi -= b.hi;
    if (old_lo < b.lo) a->hi -= 1u;
}

static void scc_pp_u64_add(SccPPU64Parts* a, SccPPU64Parts b) {
    uint32_t old_lo = a->lo;
    a->lo += b.lo;
    a->hi += b.hi;
    if (a->lo < old_lo) a->hi += 1u;
}

static void scc_pp_u64_neg(SccPPU64Parts* x) {
    x->lo = ~x->lo + 1u;
    x->hi = ~x->hi + (x->lo == 0 ? 1u : 0u);
}

static SccPPU64Parts scc_pp_u64_abs_from_i64(int64_t v) {
    SccPPU64Parts p = scc_pp_u64_parts_from_i64(v);
    if (v < 0) scc_pp_u64_neg(&p);
    return p;
}

static uint64_t scc_pp_udivmod_u64(uint64_t n, uint64_t d, uint64_t* rem_out) {
    if (d == 0) {
        if (rem_out) *rem_out = 0;
        return 0;
    }
    SccPPU64Parts nn = scc_pp_u64_parts_from_u64(n);
    SccPPU64Parts dd = scc_pp_u64_parts_from_u64(d);

    SccPPU64Parts q = (SccPPU64Parts){0, 0};
    SccPPU64Parts r = (SccPPU64Parts){0, 0};

    for (int i = 0; i < 64; i++) {
        uint32_t bit = (nn.hi >> 31);
        scc_pp_u64_shl1(&r);
        r.lo |= bit;
        scc_pp_u64_shl1(&nn);

        scc_pp_u64_shl1(&q);
        if (scc_pp_u64_ge(r, dd)) {
            scc_pp_u64_sub(&r, dd);
            q.lo |= 1u;
        }
    }

    if (rem_out) *rem_out = scc_pp_u64_from_parts(r);
    return scc_pp_u64_from_parts(q);
}

static int64_t scc_pp_mul_i64(int64_t a, int64_t b) {
    int neg = ((a < 0) ^ (b < 0)) ? 1 : 0;
    SccPPU64Parts ua = scc_pp_u64_abs_from_i64(a);
    SccPPU64Parts ub = scc_pp_u64_abs_from_i64(b);
    SccPPU64Parts acc = (SccPPU64Parts){0, 0};

    while (ub.hi || ub.lo) {
        if (ub.lo & 1u) scc_pp_u64_add(&acc, ua);
        scc_pp_u64_shl1(&ua);
        scc_pp_u64_shr1(&ub);
    }

    if (neg) scc_pp_u64_neg(&acc);
    return (int64_t)scc_pp_u64_from_parts(acc);
}

static int64_t scc_pp_div_i64(int64_t a, int64_t b) {
    if ((uint64_t)a == 0x8000000000000000ull && b == -1) return (int64_t)0x8000000000000000ull;

    int neg = ((a < 0) ^ (b < 0)) ? 1 : 0;
    SccPPU64Parts ua = scc_pp_u64_abs_from_i64(a);
    SccPPU64Parts ub = scc_pp_u64_abs_from_i64(b);
    uint64_t q = scc_pp_udivmod_u64(scc_pp_u64_from_parts(ua), scc_pp_u64_from_parts(ub), 0);

    SccPPU64Parts qq = scc_pp_u64_parts_from_u64(q);
    if (neg) scc_pp_u64_neg(&qq);
    return (int64_t)scc_pp_u64_from_parts(qq);
}

static int64_t scc_pp_mod_i64(int64_t a, int64_t b) {
    if ((uint64_t)a == 0x8000000000000000ull && b == -1) return 0;

    SccPPU64Parts ua = scc_pp_u64_abs_from_i64(a);
    SccPPU64Parts ub = scc_pp_u64_abs_from_i64(b);
    uint64_t r = 0;
    (void)scc_pp_udivmod_u64(scc_pp_u64_from_parts(ua), scc_pp_u64_from_parts(ub), &r);

    SccPPU64Parts rr = scc_pp_u64_parts_from_u64(r);
    if (a < 0) scc_pp_u64_neg(&rr);
    return (int64_t)scc_pp_u64_from_parts(rr);
}

static int64_t scc_pp_shl_i64(int64_t v, uint32_t sh) {
    if (sh >= 64) return 0;

    SccPPU64Parts x = scc_pp_u64_parts_from_i64(v);

    if (sh >= 32) {
        x.hi = x.lo << (sh - 32);
        x.lo = 0;
    } else if (sh > 0) {
        x.hi = (x.hi << sh) | (x.lo >> (32 - sh));
        x.lo = x.lo << sh;
    }

    return (int64_t)scc_pp_u64_from_parts(x);
}

static int64_t scc_pp_shr_i64(int64_t v, uint32_t sh) {
    if (sh >= 64) return (v < 0) ? -1 : 0;

    SccPPU64Parts x = scc_pp_u64_parts_from_i64(v);
    int sign = (v < 0) ? 1 : 0;

    if (sh >= 32) {
        uint32_t k = sh - 32;
        uint32_t new_lo = (k == 0) ? x.hi : (x.hi >> k);
        uint32_t new_hi = sign ? 0xFFFFFFFFu : 0u;
        if (sign && k > 0) {
            new_lo |= 0xFFFFFFFFu << (32 - k);
        }
        x.lo = new_lo;
        x.hi = new_hi;
    } else if (sh > 0) {
        uint32_t new_lo = (x.lo >> sh) | (x.hi << (32 - sh));
        uint32_t new_hi = x.hi >> sh;
        if (sign) new_hi |= 0xFFFFFFFFu << (32 - sh);
        x.lo = new_lo;
        x.hi = new_hi;
    }

    return (int64_t)scc_pp_u64_from_parts(x);
}

static int64_t scc_pp_expr_parse_mul(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_unary(e, eval);
    while (1) {
        if (scc_pp_expr_match_punct1(e, '*')) {
            int64_t r = scc_pp_expr_parse_unary(e, eval);
            v = eval ? scc_pp_mul_i64(v, r) : 0;
            continue;
        }
        if (scc_pp_expr_match_punct1(e, '/')) {
            int64_t r = scc_pp_expr_parse_unary(e, eval);
            if (eval) {
                if (r == 0) scc_pp_fatal_at(e->pp, e->file, e->src, e->line, 1, "Preprocessor: division by zero in #if");
                v = scc_pp_div_i64(v, r);
            } else {
                v = 0;
            }
            continue;
        }
        if (scc_pp_expr_match_punct1(e, '%')) {
            int64_t r = scc_pp_expr_parse_unary(e, eval);
            if (eval) {
                if (r == 0) scc_pp_fatal_at(e->pp, e->file, e->src, e->line, 1, "Preprocessor: modulo by zero in #if");
                v = scc_pp_mod_i64(v, r);
            } else {
                v = 0;
            }
            continue;
        }
        break;
    }
    return v;
}

static int64_t scc_pp_expr_parse_add(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_mul(e, eval);
    while (1) {
        if (scc_pp_expr_match_punct1(e, '+')) {
            int64_t r = scc_pp_expr_parse_mul(e, eval);
            v = eval ? (v + r) : 0;
            continue;
        }
        if (scc_pp_expr_match_punct1(e, '-')) {
            int64_t r = scc_pp_expr_parse_mul(e, eval);
            v = eval ? (v - r) : 0;
            continue;
        }
        break;
    }
    return v;
}

static int64_t scc_pp_expr_parse_shift(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_add(e, eval);
    while (1) {
        if (scc_pp_expr_match_punct2(e, '<', '<')) {
            int64_t r = scc_pp_expr_parse_add(e, eval);
            if (eval) {
                r &= 63;
                v = scc_pp_shl_i64(v, (uint32_t)r);
            } else {
                v = 0;
            }
            continue;
        }
        if (scc_pp_expr_match_punct2(e, '>', '>')) {
            int64_t r = scc_pp_expr_parse_add(e, eval);
            if (eval) {
                r &= 63;
                v = scc_pp_shr_i64(v, (uint32_t)r);
            } else {
                v = 0;
            }
            continue;
        }
        break;
    }
    return v;
}

static int64_t scc_pp_expr_parse_rel(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_shift(e, eval);
    while (1) {
        if (scc_pp_expr_match_punct2(e, '<', '=')) {
            int64_t r = scc_pp_expr_parse_shift(e, eval);
            v = eval ? ((v <= r) ? 1 : 0) : 0;
            continue;
        }
        if (scc_pp_expr_match_punct2(e, '>', '=')) {
            int64_t r = scc_pp_expr_parse_shift(e, eval);
            v = eval ? ((v >= r) ? 1 : 0) : 0;
            continue;
        }
        if (scc_pp_expr_match_punct1(e, '<')) {
            int64_t r = scc_pp_expr_parse_shift(e, eval);
            v = eval ? ((v < r) ? 1 : 0) : 0;
            continue;
        }
        if (scc_pp_expr_match_punct1(e, '>')) {
            int64_t r = scc_pp_expr_parse_shift(e, eval);
            v = eval ? ((v > r) ? 1 : 0) : 0;
            continue;
        }
        break;
    }
    return v;
}

static int64_t scc_pp_expr_parse_eq(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_rel(e, eval);
    while (1) {
        if (scc_pp_expr_match_punct2(e, '=', '=')) {
            int64_t r = scc_pp_expr_parse_rel(e, eval);
            v = eval ? ((v == r) ? 1 : 0) : 0;
            continue;
        }
        if (scc_pp_expr_match_punct2(e, '!', '=')) {
            int64_t r = scc_pp_expr_parse_rel(e, eval);
            v = eval ? ((v != r) ? 1 : 0) : 0;
            continue;
        }
        break;
    }
    return v;
}

static int64_t scc_pp_expr_parse_band(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_eq(e, eval);
    while (1) {
        scc_pp_expr_skip_ws(e);
        if (e->pos >= e->count) break;
        if (!scc_pp_tok_is_punct1(e->toks[e->pos], '&')) break;
        if (e->pos + 1 < e->count && scc_pp_tok_is_punct1(e->toks[e->pos + 1], '&')) break;
        e->pos++;
        {
            int64_t r = scc_pp_expr_parse_eq(e, eval);
            v = eval ? (v & r) : 0;
        }
    }
    return v;
}

static int64_t scc_pp_expr_parse_bxor(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_band(e, eval);
    while (scc_pp_expr_match_punct1(e, '^')) {
        int64_t r = scc_pp_expr_parse_band(e, eval);
        v = eval ? (v ^ r) : 0;
    }
    return v;
}

static int64_t scc_pp_expr_parse_bor(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_bxor(e, eval);
    while (1) {
        scc_pp_expr_skip_ws(e);
        if (e->pos >= e->count) break;
        if (!scc_pp_tok_is_punct1(e->toks[e->pos], '|')) break;
        if (e->pos + 1 < e->count && scc_pp_tok_is_punct1(e->toks[e->pos + 1], '|')) break;
        e->pos++;
        {
            int64_t r = scc_pp_expr_parse_bxor(e, eval);
            v = eval ? (v | r) : 0;
        }
    }
    return v;
}

static int64_t scc_pp_expr_parse_land(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_bor(e, eval);
    while (scc_pp_expr_match_punct2(e, '&', '&')) {
        int reval = eval && (v != 0);
        int64_t r = scc_pp_expr_parse_bor(e, reval);
        if (eval) v = (v != 0 && r != 0) ? 1 : 0;
        else v = 0;
    }
    return v;
}

static int64_t scc_pp_expr_parse_lor(SccPPExpr* e, int eval) {
    int64_t v = scc_pp_expr_parse_land(e, eval);
    while (scc_pp_expr_match_punct2(e, '|', '|')) {
        int reval = eval && (v == 0);
        int64_t r = scc_pp_expr_parse_land(e, reval);
        if (eval) v = (v != 0 || r != 0) ? 1 : 0;
        else v = 0;
    }
    return v;
}

static int64_t scc_pp_expr_parse_cond(SccPPExpr* e, int eval) {
    int64_t c = scc_pp_expr_parse_lor(e, eval);
    if (scc_pp_expr_match_punct1(e, '?')) {
        int teval = eval && (c != 0);
        int64_t t = scc_pp_expr_parse_cond(e, teval);
        if (!scc_pp_expr_match_punct1(e, ':')) scc_pp_expr_fail(e);
        int feval = eval && (c == 0);
        int64_t f = scc_pp_expr_parse_cond(e, feval);
        if (!eval) return 0;
        return (c != 0) ? t : f;
    }
    return c;
}

static int64_t scc_pp_eval_if_expr(SccPP* pp, const char* file, const char* src, int line, SccPPToken* toks, int start, int count) {
    int cap = (count - start) + 8;
    if (cap < 8) cap = 8;
    SccPPToken* tmp = (SccPPToken*)malloc((size_t)cap * sizeof(SccPPToken));
    if (!tmp) exit(1);
    int n = 0;

    for (int i = start; i < count; i++) {
        SccPPToken t = toks[i];
        if (t.kind == SCC_PP_TOK_IDENT) {
            SccPPSlice nm;
            nm.begin = t.begin;
            nm.len = t.len;
            if (scc_pp_slice_eq(nm, "defined")) {
                int j = i + 1;
                while (j < count && toks[j].kind == SCC_PP_TOK_WS) j++;
                int has_paren = 0;
                if (j < count && scc_pp_tok_is_punct1(toks[j], '(')) {
                    has_paren = 1;
                    j++;
                    while (j < count && toks[j].kind == SCC_PP_TOK_WS) j++;
                }
                if (j >= count || toks[j].kind != SCC_PP_TOK_IDENT) {
                    free(tmp);
                    scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: expected identifier after defined");
                }
                SccPPSlice dn;
                dn.begin = toks[j].begin;
                dn.len = toks[j].len;
                int isdef = scc_pp_find_macro(pp, dn) ? 1 : 0;
                j++;
                if (has_paren) {
                    while (j < count && toks[j].kind == SCC_PP_TOK_WS) j++;
                    if (j >= count || !scc_pp_tok_is_punct1(toks[j], ')')) {
                        free(tmp);
                        scc_pp_fatal_at(pp, file, src, line, 1, "Preprocessor: expected ) after defined(...)");
                    }
                    j++;
                }
                tmp[n++] = scc_pp_tok_make(SCC_PP_TOK_NUM, isdef ? "1" : "0", 1, t.line, t.col);
                i = j - 1;
                continue;
            }
        }
        tmp[n++] = t;
    }

    Buffer eb;
    buf_init(&eb, 64);
    scc_pp_expand_tokens(pp, file, src, tmp, n, &eb, line, 0);
    buf_push_u8(&eb, 0);

    free(tmp);

    SccPPToken* et = 0;
    int etc = 0;
    scc_pp_tokenize_text_no_nl((const char*)eb.data, &et, &etc);

    SccPPExpr ex;
    memset(&ex, 0, sizeof(ex));
    ex.pp = pp;
    ex.toks = et;
    ex.count = etc;
    ex.pos = 0;
    ex.file = file;
    ex.src = src;
    ex.line = line;

    int64_t v = scc_pp_expr_parse_cond(&ex, 1);
    scc_pp_expr_skip_ws(&ex);
    if (ex.pos < ex.count) {
        free(et);
        buf_free(&eb);
        scc_pp_expr_fail(&ex);
    }

    free(et);
    buf_free(&eb);
    return v;
}

static void scc_pp_args_free(SccPPArgs* a) {
    if (!a) return;
    if (a->raw) {
        for (int i = 0; i < a->count; i++) {
            if (a->raw[i]) free(a->raw[i]);
        }
        free(a->raw);
    }
    if (a->exp) {
        for (int i = 0; i < a->count; i++) {
            if (a->exp[i]) free(a->exp[i]);
        }
        free(a->exp);
    }
    memset(a, 0, sizeof(*a));
}

static void scc_pp_args_push(SccPPArgs* a, char* raw, char* exp) {
    if (a->count == a->cap) {
        int ncap = (a->cap == 0) ? 8 : (a->cap * 2);
        char** nr = (char**)realloc(a->raw, (size_t)ncap * sizeof(char*));
        char** ne = (char**)realloc(a->exp, (size_t)ncap * sizeof(char*));
        if (!nr || !ne) exit(1);
        a->raw = nr;
        a->exp = ne;
        a->cap = ncap;
    }
    a->raw[a->count] = raw;
    a->exp[a->count] = exp;
    a->count++;
}

static void scc_pp_buf_rtrim_space(Buffer* b) {
    while (b->size > 0) {
        uint8_t c = b->data[b->size - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            b->size--;
            continue;
        }
        break;
    }
}

static char* scc_pp_dup_trim_space(const char* s) {
    if (!s) return scc_pp_strndup("", 0);
    int n = (int)strlen(s);
    int i = 0;
    while (i < n && scc_pp_is_space(s[i])) i++;
    while (n > i && scc_pp_is_space(s[n - 1])) n--;
    return scc_pp_strndup(s + i, n - i);
}

static int scc_pp_parse_macro_args(SccPP* pp, const char* cur_file, const char* cur_src, SccPPToken* toks, int count, int lp_idx, int inv_line, int depth, SccPPArgs* out_args) {
    memset(out_args, 0, sizeof(*out_args));
    if (lp_idx >= count || !scc_pp_tok_is_punct1(toks[lp_idx], '(')) return -1;

    int i = lp_idx + 1;
    int arg_start = i;
    int paren_depth = 0;
    int brace_depth = 0;
    int brack_depth = 0;

    while (i < count && toks[i].kind == SCC_PP_TOK_WS) i++;
    if (i < count && scc_pp_tok_is_punct1(toks[i], ')')) {
        return i;
    }
    arg_start = i;

    while (i < count) {
        SccPPToken t = toks[i];

        if (scc_pp_tok_is_punct1(t, '(')) {
            paren_depth++;
            i++;
            continue;
        }
        if (scc_pp_tok_is_punct1(t, '{')) {
            brace_depth++;
            i++;
            continue;
        }
        if (scc_pp_tok_is_punct1(t, '[')) {
            brack_depth++;
            i++;
            continue;
        }
        if (scc_pp_tok_is_punct1(t, ')')) {
            if (paren_depth == 0 && brace_depth == 0 && brack_depth == 0) {
                int ts = arg_start;
                int te = i;
                scc_pp_trim_ws_range(toks, &ts, &te);

                char* raw = scc_pp_tokens_to_text_dup(toks, ts, te);

                Buffer eb;
                buf_init(&eb, 64);
                scc_pp_expand_tokens(pp, cur_file, cur_src, toks + ts, te - ts, &eb, inv_line, depth + 1);
                buf_push_u8(&eb, 0);
                char* exp = (char*)eb.data;

                scc_pp_args_push(out_args, raw, exp);
                return i;
            }
            paren_depth--;
            i++;
            continue;
        }
        if (scc_pp_tok_is_punct1(t, '}')) {
            if (brace_depth > 0) brace_depth--;
            i++;
            continue;
        }
        if (scc_pp_tok_is_punct1(t, ']')) {
            if (brack_depth > 0) brack_depth--;
            i++;
            continue;
        }

        if (scc_pp_tok_is_punct1(t, ',') && paren_depth == 0 && brace_depth == 0 && brack_depth == 0) {
            int ts = arg_start;
            int te = i;
            scc_pp_trim_ws_range(toks, &ts, &te);

            char* raw = scc_pp_tokens_to_text_dup(toks, ts, te);
            Buffer eb;
            buf_init(&eb, 64);
            scc_pp_expand_tokens(pp, cur_file, cur_src, toks + ts, te - ts, &eb, inv_line, depth + 1);
            buf_push_u8(&eb, 0);
            char* exp = (char*)eb.data;
            scc_pp_args_push(out_args, raw, exp);

            i++;
            while (i < count && toks[i].kind == SCC_PP_TOK_WS) i++;
            arg_start = i;
            continue;
        }

        i++;
    }

    scc_pp_fatal_at(pp, cur_file, cur_src, toks[lp_idx].line, toks[lp_idx].col, "Preprocessor: unterminated macro invocation");
    return -1;
}

static char* scc_pp_join_va_args(const char** parts, int n) {
    Buffer b;
    buf_init(&b, 64);
    for (int i = 0; i < n; i++) {
        if (i != 0) scc_pp_emit_cstr(&b, ", ");
        if (parts[i]) scc_pp_emit_cstr(&b, parts[i]);
    }
    buf_push_u8(&b, 0);
    return (char*)b.data;
}

static void scc_pp_expand_func_macro(SccPP* pp, const char* cur_file, const char* cur_src, SccPPMacro* m, SccPPArgs* args, Buffer* out, int inv_line, int depth) {
    int fixed = m->is_variadic ? (m->param_count - 1) : m->param_count;
    if (fixed < 0) fixed = 0;

    if (!m->is_variadic) {
        if (args->count != m->param_count) {
            scc_pp_fatal_at(pp, cur_file, cur_src, inv_line, 1, "Preprocessor: wrong number of macro arguments");
        }
    } else {
        if (args->count < fixed) {
            scc_pp_fatal_at(pp, cur_file, cur_src, inv_line, 1, "Preprocessor: wrong number of macro arguments");
        }
    }

    const char** rawv = 0;
    const char** expv = 0;
    if (m->param_count > 0) {
        rawv = (const char**)malloc((size_t)m->param_count * sizeof(const char*));
        expv = (const char**)malloc((size_t)m->param_count * sizeof(const char*));
        if (!rawv || !expv) exit(1);
        for (int i = 0; i < m->param_count; i++) {
            rawv[i] = "";
            expv[i] = "";
        }
    }

    for (int i = 0; i < fixed && i < args->count; i++) {
        if (rawv) rawv[i] = args->raw[i] ? args->raw[i] : "";
        if (expv) expv[i] = args->exp[i] ? args->exp[i] : "";
    }

    char* va_raw = 0;
    char* va_exp = 0;
    if (m->is_variadic) {
        if (m->param_count <= 0) {
            scc_pp_fatal_at(pp, cur_file, cur_src, inv_line, 1, "Preprocessor: invalid variadic macro definition");
        }
        int extra = args->count - fixed;
        if (extra <= 0) {
            va_raw = scc_pp_strndup("", 0);
            va_exp = scc_pp_strndup("", 0);
        } else {
            const char** rr = (const char**)malloc((size_t)extra * sizeof(const char*));
            const char** ee = (const char**)malloc((size_t)extra * sizeof(const char*));
            if (!rr || !ee) exit(1);
            for (int k = 0; k < extra; k++) {
                rr[k] = args->raw[fixed + k] ? args->raw[fixed + k] : "";
                ee[k] = args->exp[fixed + k] ? args->exp[fixed + k] : "";
            }
            va_raw = scc_pp_join_va_args(rr, extra);
            va_exp = scc_pp_join_va_args(ee, extra);
            free(rr);
            free(ee);
        }

        rawv[m->param_count - 1] = va_raw;
        expv[m->param_count - 1] = va_exp;
    }

    Buffer tmp;
    buf_init(&tmp, 64);

    for (int i = 0; i < m->repl_count; i++) {
        SccPPToken t = m->repl[i];

        if (t.kind == SCC_PP_TOK_HASH) {
            int j = i + 1;
            while (j < m->repl_count && m->repl[j].kind == SCC_PP_TOK_WS) j++;
            if (j < m->repl_count && m->repl[j].kind == SCC_PP_TOK_IDENT) {
                SccPPSlice nm;
                nm.begin = m->repl[j].begin;
                nm.len = m->repl[j].len;
                int pi = scc_pp_param_index(m, nm);
                if (pi >= 0) {
                    char* str = scc_pp_stringize_text_dup(rawv[pi]);
                    scc_pp_emit_cstr(&tmp, str);
                    free(str);
                    i = j;
                    continue;
                }
            }
            scc_pp_fatal_at(pp, cur_file, cur_src, inv_line, 1, "Preprocessor: invalid # operator in macro replacement list");
        }

        if (t.kind == SCC_PP_TOK_HASHHASH) {
            scc_pp_buf_rtrim_space(&tmp);
            {
                int has_left = 0;
                for (int k = i - 1; k >= 0; k--) {
                    if (m->repl[k].kind == SCC_PP_TOK_WS) continue;
                    has_left = 1;
                    break;
                }
                if (!has_left) {
                    scc_pp_fatal_at(pp, cur_file, cur_src, inv_line, 1, "Preprocessor: invalid ## operator in macro replacement list");
                }
            }
            int j = i + 1;
            while (j < m->repl_count && m->repl[j].kind == SCC_PP_TOK_WS) j++;
            if (j >= m->repl_count) {
                scc_pp_fatal_at(pp, cur_file, cur_src, inv_line, 1, "Preprocessor: invalid ## operator in macro replacement list");
            }

            SccPPToken r = m->repl[j];
            if (r.kind == SCC_PP_TOK_IDENT) {
                SccPPSlice nm;
                nm.begin = r.begin;
                nm.len = r.len;
                int pi = scc_pp_param_index(m, nm);
                if (pi >= 0) {
                    char* tr = scc_pp_dup_trim_space(rawv[pi]);
                    scc_pp_emit_cstr(&tmp, tr);
                    free(tr);
                    i = j;
                    continue;
                }
            }

            scc_pp_emit(&tmp, r.begin, r.len);
            i = j;
            continue;
        }

        if (t.kind == SCC_PP_TOK_IDENT) {
            SccPPSlice nm;
            nm.begin = t.begin;
            nm.len = t.len;
            int pi = scc_pp_param_index(m, nm);
            if (pi >= 0) {
                int j = i + 1;
                while (j < m->repl_count && m->repl[j].kind == SCC_PP_TOK_WS) j++;
                if (j < m->repl_count && m->repl[j].kind == SCC_PP_TOK_HASHHASH) {
                    char* tr = scc_pp_dup_trim_space(rawv ? rawv[pi] : "");
                    scc_pp_emit_cstr(&tmp, tr);
                    free(tr);
                } else {
                    scc_pp_emit_cstr(&tmp, expv ? expv[pi] : "");
                }
                continue;
            }
        }

        scc_pp_emit(&tmp, t.begin, t.len);
    }

    buf_push_u8(&tmp, 0);

    SccPPToken* tt = 0;
    int ttc = 0;
    scc_pp_tokenize_text_no_nl((const char*)tmp.data, &tt, &ttc);
    scc_pp_expand_tokens(pp, cur_file, cur_src, tt, ttc, out, inv_line, depth + 1);
    free(tt);
    buf_free(&tmp);

    if (va_raw) free(va_raw);
    if (va_exp) free(va_exp);
    if (rawv) free(rawv);
    if (expv) free(expv);
}

static void scc_pp_define_func_like(SccPP* pp, const char* name, char** params, int param_count, int is_variadic, const char* body) {
    SccPPSlice nm;
    nm.begin = name;
    nm.len = (int)strlen(name);
    scc_pp_undef(pp, nm);

    if (pp->macro_count == pp->macro_cap) {
        int ncap = (pp->macro_cap == 0) ? 64 : (pp->macro_cap * 2);
        SccPPMacro* nd = (SccPPMacro*)realloc(pp->macros, (size_t)ncap * sizeof(SccPPMacro));
        if (!nd) exit(1);
        memset(nd + pp->macro_cap, 0, (size_t)(ncap - pp->macro_cap) * sizeof(SccPPMacro));
        pp->macros = nd;
        pp->macro_cap = ncap;
    }

    SccPPMacro* mcr = &pp->macros[pp->macro_count++];
    memset(mcr, 0, sizeof(*mcr));
    mcr->name = scc_pp_strndup(name, (int)strlen(name));
    mcr->is_func = 1;
    mcr->is_variadic = is_variadic;
    mcr->param_count = param_count;
    mcr->params = params;
    mcr->repl_src = 0;
    mcr->repl = 0;
    mcr->repl_count = 0;
    mcr->repl_cap = 0;
    scc_pp_macro_set_repl_from_text(mcr, body);
}

static void scc_pp_define_obj_like(SccPP* pp, const char* name, const char* value) {
    SccPPSlice nm;
    nm.begin = name;
    nm.len = (int)strlen(name);
    scc_pp_undef(pp, nm);

    if (pp->macro_count == pp->macro_cap) {
        int ncap = (pp->macro_cap == 0) ? 64 : (pp->macro_cap * 2);
        SccPPMacro* nd = (SccPPMacro*)realloc(pp->macros, (size_t)ncap * sizeof(SccPPMacro));
        if (!nd) exit(1);
        memset(nd + pp->macro_cap, 0, (size_t)(ncap - pp->macro_cap) * sizeof(SccPPMacro));
        pp->macros = nd;
        pp->macro_cap = ncap;
    }

    SccPPMacro* m = &pp->macros[pp->macro_count++];
    memset(m, 0, sizeof(*m));
    m->name = scc_pp_strndup(name, (int)strlen(name));
    m->is_func = 0;
    m->is_variadic = 0;
    m->param_count = 0;
    m->params = 0;
    m->repl_src = 0;
    m->repl = 0;
    m->repl_count = 0;
    m->repl_cap = 0;
    scc_pp_macro_set_repl_from_text(m, value);
}

static int scc_pp_is_once_file(SccPP* pp, const char* path) {
    for (int i = 0; i < pp->once_count; i++) {
        if (pp->once_files[i] && strcmp(pp->once_files[i], path) == 0) return 1;
    }
    return 0;
}

static void scc_pp_mark_once(SccPP* pp, const char* path) {
    if (!path || !path[0]) return;
    if (scc_pp_is_once_file(pp, path)) return;

    if (pp->once_count == pp->once_cap) {
        int ncap = (pp->once_cap == 0) ? 32 : (pp->once_cap * 2);
        char** nd = (char**)realloc(pp->once_files, (size_t)ncap * sizeof(char*));
        if (!nd) exit(1);
        pp->once_files = nd;
        pp->once_cap = ncap;
    }
    pp->once_files[pp->once_count++] = scc_pp_strndup(path, (int)strlen(path));
}

static void scc_pp_emit_string_literal_escaped(Buffer* out, const char* s) {
    buf_push_u8(out, (uint8_t)'"');
    if (s) {
        for (int i = 0; s[i]; i++) {
            char c = s[i];
            if (c == '"' || c == '\\') {
                buf_push_u8(out, (uint8_t)'\\');
                buf_push_u8(out, (uint8_t)c);
            } else {
                buf_push_u8(out, (uint8_t)c);
            }
        }
    }
    buf_push_u8(out, (uint8_t)'"');
}

static int scc_pp_try_open(const char* path) {
    int fd = open(path, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static char* scc_pp_resolve_include(SccPP* pp, const char* cur_file, const char* inc, int is_angle) {
    if (!inc || !inc[0]) return 0;

    if (!is_angle && cur_file && cur_file[0]) {
        char* dir = scc_pp_dirname_dup(cur_file);
        char* p = scc_pp_path_join(dir, inc);
        free(dir);
        if (scc_pp_try_open(p)) return p;
        free(p);
    }

    if (pp->cfg && pp->cfg->include_paths) {
        for (int i = 0; i < pp->cfg->include_path_count; i++) {
            const char* d = pp->cfg->include_paths[i];
            if (!d) continue;
            char* p = scc_pp_path_join(d, inc);
            if (scc_pp_try_open(p)) return p;
            free(p);
        }
    }

    return 0;
}

static char* scc_pp_unquote_string_token(SccPPToken t) {
    if (t.kind != SCC_PP_TOK_STR) return 0;
    if (t.len < 2) return scc_pp_strndup("", 0);
    const char* b = t.begin;
    if (b[0] != '"' || b[t.len - 1] != '"') {
        return scc_pp_strndup(b, t.len);
    }
    return scc_pp_strndup(b + 1, t.len - 2);
}

static int scc_pp_collect_line_tokens(SccPPScanner* sc, SccPPToken** out_toks, int* out_count) {
    int cap = 32;
    int cnt = 0;
    SccPPToken* toks = (SccPPToken*)malloc((size_t)cap * sizeof(SccPPToken));
    if (!toks) exit(1);

    int got_nl = 0;
    while (1) {
        SccPPToken t = scc_pp_next_token(sc);
        if (t.kind == SCC_PP_TOK_EOF) break;
        if (t.kind == SCC_PP_TOK_NL) {
            got_nl = 1;
            break;
        }
        if (cnt == cap) {
            cap *= 2;
            SccPPToken* nt = (SccPPToken*)realloc(toks, (size_t)cap * sizeof(SccPPToken));
            if (!nt) exit(1);
            toks = nt;
        }
        toks[cnt++] = t;
    }

    *out_toks = toks;
    *out_count = cnt;
    return got_nl;
}

static char* scc_pp_concat_tokens_dup(SccPPToken* toks, int start, int count) {
    Buffer b;
    buf_init(&b, 64);
    for (int i = start; i < count; i++) {
        scc_pp_emit(&b, toks[i].begin, toks[i].len);
    }
    buf_push_u8(&b, 0);
    return (char*)b.data;
}

static void scc_pp_expand_tokens(SccPP* pp, const char* cur_file, const char* cur_src, SccPPToken* toks, int count, Buffer* out, int inv_line, int depth) {
    if (depth > 64) {
        scc_pp_fatal_at(pp, cur_file, cur_src, 1, 1, "Preprocessor: macro expansion too deep");
    }

    for (int i = 0; i < count; i++) {
        SccPPToken t = toks[i];

        if (t.kind == SCC_PP_TOK_IDENT) {
            SccPPSlice nm;
            nm.begin = t.begin;
            nm.len = t.len;

            if (scc_pp_slice_eq(nm, "__FILE__")) {
                scc_pp_emit_string_literal_escaped(out, cur_file ? cur_file : "<input>");
                continue;
            }
            if (scc_pp_slice_eq(nm, "__LINE__")) {
                scc_pp_emit_u32_dec(out, (uint32_t)(inv_line > 0 ? inv_line : t.line));
                continue;
            }
            if (scc_pp_slice_eq(nm, "__STDC__")) {
                scc_pp_emit_u32_dec(out, 1);
                continue;
            }
            if (scc_pp_slice_eq(nm, "__STDC_HOSTED__")) {
                scc_pp_emit_u32_dec(out, 0);
                continue;
            }
            if (scc_pp_slice_eq(nm, "__STDC_VERSION__")) {
                scc_pp_emit_cstr(out, "199901L");
                continue;
            }
            if (scc_pp_slice_eq(nm, "__DATE__")) {
                scc_pp_emit_string_literal_escaped(out, __DATE__);
                continue;
            }
            if (scc_pp_slice_eq(nm, "__TIME__")) {
                scc_pp_emit_string_literal_escaped(out, __TIME__);
                continue;
            }

            SccPPMacro* m = scc_pp_find_macro(pp, nm);
            if (m && !scc_pp_is_expanding(pp, m->name)) {
                int call_line = (inv_line > 0 ? inv_line : t.line);
                if (!m->is_func) {
                    scc_pp_push_expanding(pp, m->name);
                    scc_pp_expand_tokens(pp, cur_file, cur_src, m->repl, m->repl_count, out, call_line, depth + 1);
                    scc_pp_pop_expanding(pp);
                    continue;
                }

                int j = i + 1;
                while (j < count && toks[j].kind == SCC_PP_TOK_WS) j++;
                if (j < count && scc_pp_tok_is_punct1(toks[j], '(')) {
                    SccPPArgs args;
                    int rp = scc_pp_parse_macro_args(pp, cur_file, cur_src, toks, count, j, call_line, depth, &args);
                    if (rp < 0) {
                        scc_pp_args_free(&args);
                        break;
                    }

                    scc_pp_push_expanding(pp, m->name);
                    scc_pp_expand_func_macro(pp, cur_file, cur_src, m, &args, out, call_line, depth + 1);
                    scc_pp_pop_expanding(pp);
                    scc_pp_args_free(&args);
                    i = rp;
                    continue;
                }
            }
        }

        scc_pp_emit(out, t.begin, t.len);
    }
}

static void scc_pp_process_file_internal(SccPP* pp, const char* path, Buffer* out, int depth) {
    scc_pp_include_push(pp, path);
    if (depth > pp->max_include_depth) {
        scc_pp_fatal_at(pp, path, 0, 1, 1, "Preprocessor: include nesting too deep");
    }
    if (scc_pp_is_once_file(pp, path)) {
        scc_pp_include_pop(pp);
        return;
    }

    int line_bias = 0;
    const char* logical_file = path;
    char* logical_file_alloc = 0;

    Buffer src_storage;
    char* src = scc_pp_read_entire_file(path, &src_storage);
    if (!src) {
        scc_pp_fatal_at(pp, path, 0, 1, 1, "Preprocessor: cannot open input file");
    }

    SccPPScanner sc;
    memset(&sc, 0, sizeof(sc));
    sc.pp = pp;
    sc.file = path;
    sc.src = src;
    sc.pos = 0;
    sc.line = 1;
    sc.col = 1;

    while (1) {
        int phys_line_no = sc.line;
        int line_no = phys_line_no + line_bias;
        SccPPToken* line_toks = 0;
        int line_count = 0;
        int had_nl = scc_pp_collect_line_tokens(&sc, &line_toks, &line_count);
        if (line_count == 0 && !had_nl && scc_pp_sc_cur(&sc) == 0) {
            free(line_toks);
            break;
        }

        for (int k = 0; k < line_count; k++) {
            line_toks[k].line += line_bias;
        }

        int i = 0;
        while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;

        int cur_active = scc_pp_is_active(pp);

        int is_directive = (i < line_count && line_toks[i].kind == SCC_PP_TOK_HASH);
        if (is_directive) {
            i++;
            while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;

            if (i < line_count && line_toks[i].kind == SCC_PP_TOK_IDENT) {
                SccPPSlice dir;
                dir.begin = line_toks[i].begin;
                dir.len = line_toks[i].len;
                i++;

                if (scc_pp_slice_eq(dir, "if")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    int parent_active = scc_pp_is_active(pp);
                    int cond_true = 0;
                    if (parent_active) {
                        cond_true = (scc_pp_eval_if_expr(pp, logical_file, src, line_no, line_toks, i, line_count) != 0);
                    }
                    scc_pp_if_push(pp, parent_active, cond_true);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "ifdef")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i >= line_count || line_toks[i].kind != SCC_PP_TOK_IDENT) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected identifier after #ifdef");
                    }
                    SccPPSlice nm;
                    nm.begin = line_toks[i].begin;
                    nm.len = line_toks[i].len;
                    i++;
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i < line_count) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #ifdef");
                    }
                    int parent_active = scc_pp_is_active(pp);
                    int cond_true = 0;
                    if (parent_active) cond_true = scc_pp_find_macro(pp, nm) ? 1 : 0;
                    scc_pp_if_push(pp, parent_active, cond_true);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "ifndef")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i >= line_count || line_toks[i].kind != SCC_PP_TOK_IDENT) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected identifier after #ifndef");
                    }
                    SccPPSlice nm;
                    nm.begin = line_toks[i].begin;
                    nm.len = line_toks[i].len;
                    i++;
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i < line_count) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #ifndef");
                    }
                    int parent_active = scc_pp_is_active(pp);
                    int cond_true = 0;
                    if (parent_active) cond_true = scc_pp_find_macro(pp, nm) ? 0 : 1;
                    scc_pp_if_push(pp, parent_active, cond_true);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "elif")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    int cond_true = 0;
                    if (pp->if_count > 0 && pp->ifs[pp->if_count - 1].parent_active && !pp->ifs[pp->if_count - 1].any_true) {
                        cond_true = (scc_pp_eval_if_expr(pp, logical_file, src, line_no, line_toks, i, line_count) != 0);
                    }
                    scc_pp_if_elif(pp, logical_file, src, line_no, cond_true);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "else")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i < line_count) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #else");
                    }
                    scc_pp_if_else(pp, logical_file, src, line_no);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "endif")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i < line_count) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #endif");
                    }
                    scc_pp_if_pop(pp, logical_file, src, line_no);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (!cur_active) {
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "line")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;

                    Buffer eb;
                    buf_init(&eb, 64);
                    scc_pp_expand_tokens(pp, logical_file, src, line_toks + i, line_count - i, &eb, line_no, 0);
                    buf_push_u8(&eb, 0);

                    SccPPToken* et = 0;
                    int etc = 0;
                    scc_pp_tokenize_text_no_nl((const char*)eb.data, &et, &etc);

                    int j = 0;
                    while (j < etc && et[j].kind == SCC_PP_TOK_WS) j++;
                    if (j >= etc || !scc_pp_tok_is_dec_digit_seq(et[j])) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected line number after #line");
                    }

                    int64_t new_line = scc_pp_parse_dec_digit_seq_token(et[j]);
                    if (new_line <= 0) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: invalid line number in #line");
                    }
                    j++;
                    while (j < etc && et[j].kind == SCC_PP_TOK_WS) j++;
                    if (j < etc && et[j].kind == SCC_PP_TOK_STR) {
                        char* nf = scc_pp_unquote_string_token(et[j]);
                        if (logical_file_alloc) free(logical_file_alloc);
                        logical_file_alloc = nf;
                        logical_file = logical_file_alloc;
                        j++;
                    }

                    while (j < etc && et[j].kind == SCC_PP_TOK_WS) j++;
                    if (j < etc) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #line");
                    }

                    line_bias = (int)new_line - (phys_line_no + 1);

                    free(et);
                    buf_free(&eb);

                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "error")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    char* txt = scc_pp_concat_tokens_dup(line_toks, i, line_count);
                    Buffer msg;
                    buf_init(&msg, 64);
                    scc_pp_emit_cstr(&msg, "Preprocessor: #error");
                    if (txt && txt[0]) {
                        scc_pp_emit_cstr(&msg, " ");
                        scc_pp_emit_cstr(&msg, txt);
                    }
                    buf_push_u8(&msg, 0);
                    scc_pp_fatal_at(pp, logical_file, src, line_no, 1, (const char*)msg.data);
                }

                if (scc_pp_slice_eq(dir, "include")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;

                    char* inc = 0;
                    int is_angle = 0;
                    if (i < line_count && line_toks[i].kind == SCC_PP_TOK_STR) {
                        inc = scc_pp_unquote_string_token(line_toks[i]);
                        is_angle = 0;
                        i++;
                        while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                        if (i < line_count) {
                            scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #include");
                        }
                    } else if (i < line_count && line_toks[i].kind == SCC_PP_TOK_PUNCT && line_toks[i].len == 1 && line_toks[i].begin[0] == '<') {
                        i++;
                        int start = i;
                        while (i < line_count) {
                            if (line_toks[i].kind == SCC_PP_TOK_PUNCT && line_toks[i].len == 1 && line_toks[i].begin[0] == '>') break;
                            i++;
                        }
                        if (i >= line_count) {
                            scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: unterminated <...> in #include");
                        }
                        const char* b = line_toks[start].begin;
                        const char* e = line_toks[i].begin;
                        inc = scc_pp_strndup(b, (int)(e - b));
                        is_angle = 1;
                        i++;
                        while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                        if (i < line_count) {
                            scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #include<>");
                        }
                    } else {
                        Buffer eb;
                        buf_init(&eb, 64);
                        scc_pp_expand_tokens(pp, logical_file, src, line_toks + i, line_count - i, &eb, line_no, 0);
                        buf_push_u8(&eb, 0);

                        SccPPToken* et = 0;
                        int etc = 0;
                        scc_pp_tokenize_text_no_nl((const char*)eb.data, &et, &etc);

                        int j = 0;
                        while (j < etc && et[j].kind == SCC_PP_TOK_WS) j++;
                        if (j < etc && et[j].kind == SCC_PP_TOK_STR) {
                            inc = scc_pp_unquote_string_token(et[j]);
                            is_angle = 0;
                            j++;
                            while (j < etc && et[j].kind == SCC_PP_TOK_WS) j++;
                            if (j < etc) {
                                scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after macro-expanded #include");
                            }
                        } else if (j < etc && et[j].kind == SCC_PP_TOK_PUNCT && et[j].len == 1 && et[j].begin[0] == '<') {
                            j++;
                            int start = j;
                            while (j < etc) {
                                if (et[j].kind == SCC_PP_TOK_PUNCT && et[j].len == 1 && et[j].begin[0] == '>') break;
                                j++;
                            }
                            if (j >= etc) {
                                scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: unterminated <...> in #include");
                            }
                            const char* b = et[start].begin;
                            const char* e = et[j].begin;
                            inc = scc_pp_strndup(b, (int)(e - b));
                            is_angle = 1;
                            j++;
                            while (j < etc && et[j].kind == SCC_PP_TOK_WS) j++;
                            if (j < etc) {
                                scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after macro-expanded #include<>");
                            }
                        } else {
                            scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected \"file\" or <file> after macro expansion in #include");
                        }

                        free(et);
                        buf_free(&eb);
                    }

                    char* resolved = scc_pp_resolve_include(pp, path, inc, is_angle);
                    if (!resolved) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: include file not found");
                    }

                    int skip = scc_pp_is_once_file(pp, resolved);
                    if (!skip) {
                        scc_pp_process_file_internal(pp, resolved, out, depth + 1);
                    }

                    free(inc);
                    free(resolved);
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "pragma")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i < line_count && line_toks[i].kind == SCC_PP_TOK_IDENT) {
                        SccPPSlice arg;
                        arg.begin = line_toks[i].begin;
                        arg.len = line_toks[i].len;
                        if (scc_pp_slice_eq(arg, "once")) {
                            if (pp->cfg && pp->cfg->allow_extensions) {
                                scc_pp_mark_once(pp, path);
                            }
                        }
                    }
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "undef")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i >= line_count || line_toks[i].kind != SCC_PP_TOK_IDENT) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected identifier after #undef");
                    }
                    SccPPSlice nm;
                    nm.begin = line_toks[i].begin;
                    nm.len = line_toks[i].len;
                    i++;
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i < line_count) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: trailing tokens after #undef");
                    }
                    scc_pp_undef(pp, nm);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                if (scc_pp_slice_eq(dir, "define")) {
                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    if (i >= line_count || line_toks[i].kind != SCC_PP_TOK_IDENT) {
                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected identifier after #define");
                    }

                    SccPPToken name_tok = line_toks[i++];
                    char* name = scc_pp_strndup(name_tok.begin, name_tok.len);

                    if (i < line_count && scc_pp_tok_is_punct1(line_toks[i], '(')) {
                        i++;

                        int is_variadic = 0;
                        int param_count = 0;
                        int param_cap = 0;
                        char** params = 0;

                        while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                        if (i < line_count && scc_pp_tok_is_punct1(line_toks[i], ')')) {
                            i++;
                        } else {
                            while (1) {
                                while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;

                                if (scc_pp_tok_is_ellipsis(line_toks, i, line_count)) {
                                    is_variadic = 1;
                                    if (param_count == param_cap) {
                                        int ncap = (param_cap == 0) ? 8 : (param_cap * 2);
                                        char** nd = (char**)realloc(params, (size_t)ncap * sizeof(char*));
                                        if (!nd) exit(1);
                                        params = nd;
                                        param_cap = ncap;
                                    }
                                    params[param_count++] = scc_pp_strndup("__VA_ARGS__", 11);
                                    i += 3;
                                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                                    if (i >= line_count || !scc_pp_tok_is_punct1(line_toks[i], ')')) {
                                        scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected ) after ... in macro parameters");
                                    }
                                    i++;
                                    break;
                                }

                                if (i >= line_count || line_toks[i].kind != SCC_PP_TOK_IDENT) {
                                    scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected parameter name");
                                }

                                if (param_count == param_cap) {
                                    int ncap = (param_cap == 0) ? 8 : (param_cap * 2);
                                    char** nd = (char**)realloc(params, (size_t)ncap * sizeof(char*));
                                    if (!nd) exit(1);
                                    params = nd;
                                    param_cap = ncap;
                                }
                                params[param_count++] = scc_pp_strndup(line_toks[i].begin, line_toks[i].len);
                                i++;

                                while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                                if (i < line_count && scc_pp_tok_is_punct1(line_toks[i], ',')) {
                                    i++;
                                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                                    if (scc_pp_tok_is_ellipsis(line_toks, i, line_count)) {
                                        is_variadic = 1;
                                        if (param_count == param_cap) {
                                            int ncap = (param_cap == 0) ? 8 : (param_cap * 2);
                                            char** nd = (char**)realloc(params, (size_t)ncap * sizeof(char*));
                                            if (!nd) exit(1);
                                            params = nd;
                                            param_cap = ncap;
                                        }
                                        params[param_count++] = scc_pp_strndup("__VA_ARGS__", 11);
                                        i += 3;
                                        while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                                        if (i >= line_count || !scc_pp_tok_is_punct1(line_toks[i], ')')) {
                                            scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected ) after ... in macro parameters");
                                        }
                                        i++;
                                        break;
                                    }
                                    continue;
                                }
                                if (i < line_count && scc_pp_tok_is_punct1(line_toks[i], ')')) {
                                    i++;
                                    break;
                                }
                                scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: expected , or ) in macro parameters");
                            }
                        }

                        while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                        char* body = scc_pp_concat_tokens_dup(line_toks, i, line_count);
                        scc_pp_define_func_like(pp, name, params, param_count, is_variadic, body);
                        free(name);
                        free(body);
                        if (had_nl) scc_pp_emit_cstr(out, "\n");
                        free(line_toks);
                        continue;
                    }

                    while (i < line_count && line_toks[i].kind == SCC_PP_TOK_WS) i++;
                    char* body = scc_pp_concat_tokens_dup(line_toks, i, line_count);
                    scc_pp_define_obj_like(pp, name, body);
                    free(name);
                    free(body);
                    if (had_nl) scc_pp_emit_cstr(out, "\n");
                    free(line_toks);
                    continue;
                }

                scc_pp_fatal_at(pp, logical_file, src, line_no, 1, "Preprocessor: unknown directive");
            }

            if (had_nl) scc_pp_emit_cstr(out, "\n");
            free(line_toks);
            continue;
        }

        if (!cur_active) {
            if (had_nl) scc_pp_emit_cstr(out, "\n");
            free(line_toks);
            if (!had_nl && scc_pp_sc_cur(&sc) == 0) break;
            continue;
        }

        scc_pp_expand_tokens(pp, logical_file, src, line_toks, line_count, out, 0, 0);
        if (had_nl) scc_pp_emit_cstr(out, "\n");
        free(line_toks);

        if (!had_nl && scc_pp_sc_cur(&sc) == 0) break;
    }

    buf_free(&src_storage);
    if (logical_file_alloc) free(logical_file_alloc);
    scc_pp_include_pop(pp);
}

static SccPPResult scc_preprocess_file(const SccPPConfig* cfg, const char* input_path) {
    SccPPResult r;
    memset(&r, 0, sizeof(r));
    r.ok = 0;
    r.text = 0;

    if (!input_path) return r;

    SccPP pp;
    scc_pp_init(&pp, cfg);

    if (cfg && cfg->defines) {
        for (int i = 0; i < cfg->define_count; i++) {
            if (!cfg->defines[i].name) continue;
            scc_pp_define_obj_like(&pp, cfg->defines[i].name, cfg->defines[i].value);
        }
    }

    Buffer out;
    buf_init(&out, 4096);
    scc_pp_process_file_internal(&pp, input_path, &out, 0);

    if (pp.if_count != 0) {
        scc_pp_fatal_at(&pp, input_path, 0, 1, 1, "Preprocessor: missing #endif");
    }
    buf_push_u8(&out, 0);

    r.ok = 1;
    r.text = (char*)out.data;

    scc_pp_free(&pp);
    return r;
}

#endif

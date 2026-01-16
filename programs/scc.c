// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

// SCC - Small C Compiler

#include "scc/scc_common.h"
#include "scc/scc_elf.h"

#include "scc/scc_buffer.h"

#include "scc/scc_lexer.h"

#include "scc/scc_core.h"

#include "scc/scc_ast.h"

#include "scc/scc_parser.h"

#include "scc/scc_x86.h"
#include "scc/scc_obj_writer.h"
#include "scc/scc_consteval.h"
#include "scc/scc_codegen.h"

#include "scc/scc_ir.h"
#include "scc/scc_ir_lower.h"
#include "scc/scc_ir_x86.h"

#include "scc/scc_pp.h"

static void scc_compile_file(const char* in_path, const char* out_path, const SccPPConfig* pp_cfg) {
    SccPPResult pp_res = scc_preprocess_file(pp_cfg, in_path);
    if (!pp_res.ok || !pp_res.text) exit(1);

    Arena arena;
    arena_init(&arena, 16 * 1024);

    SymTable syms;
    symtab_init(&syms);

    Parser p;
    memset(&p, 0, sizeof(p));
    p.file = in_path;
    p.src = pp_res.text;
    p.lx.file = in_path;
    p.lx.src = pp_res.text;
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

    SccConstEval ce;
    memset(&ce, 0, sizeof(ce));
    ce.p = &p;
    ce.syms = &syms;
    ce.data = &data;
    ce.str_id = cg.str_id;

    uint32_t bss_size = 0;

    for (AstGlobal* g = u->first_global; g; g = g->next) {
        if (!g->sym) continue;
        if (g->sym->shndx == SHN_UNDEF) continue;

        uint32_t sz = type_size(g->ty);
        uint32_t al = type_align(g->ty);

        if (g->init) {
            uint32_t aligned = align_up_u32(data.size, al);
            while (data.size < aligned) buf_push_u8(&data, 0);

            g->sym->shndx = 2;
            g->sym->value = data.size;
            g->sym->size = sz;

            uint32_t v = 0;
            Symbol* rs = 0;
            scc_eval_const_u32(&ce, g->init, &v, &rs);
            cg.str_id = ce.str_id;

            if (sz == 1) {
                if (rs) scc_fatal_at(p.file, p.src, g->init->tok.line, g->init->tok.col, "Relocation is not supported for 1-byte global initializer");
                if (g->ty && g->ty->kind == TYPE_BOOL) buf_push_u8(&data, (v != 0) ? 1u : 0u);
                else buf_push_u8(&data, (uint8_t)v);
            } else if (sz == 2) {
                if (rs) scc_fatal_at(p.file, p.src, g->init->tok.line, g->init->tok.col, "Relocation is not supported for 2-byte global initializer");
                buf_push_u16(&data, (uint16_t)(v & 0xFFFFu));
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

    IrModule m;
    ir_module_init(&m, &arena);
    ir_lower_unit_stub(&m, &p, &syms, &data, &cg.str_id, u);

    IrX86Ctx ix;
    memset(&ix, 0, sizeof(ix));
    ix.text = &text;
    ix.data = &data;
    ix.rel_text = &rel_text;
    ix.rel_data = &rel_data;
    ix.syms = &syms;
    ir_x86_codegen_module_stub(&ix, &m);

    write_elf_object(out_path, &text, &data, bss_size, &rel_text, &rel_data, &syms);

    buf_free(&text);
    buf_free(&data);
    buf_free(&rel_text);
    buf_free(&rel_data);
    symtab_free(&syms);
    arena_free(&arena);
    free(pp_res.text);
}

static void scc_opt_push_cstr(const char* s, char*** out, int* out_count, int* out_cap) {
    if (!s) return;
    if (*out_count == *out_cap) {
        int ncap = (*out_cap == 0) ? 8 : (*out_cap * 2);
        char** nd = (char**)realloc(*out, (size_t)ncap * sizeof(char*));
        if (!nd) exit(1);
        *out = nd;
        *out_cap = ncap;
    }
    (*out)[(*out_count)++] = strdup(s);
}

static void scc_opt_push_define(const char* s, SccPPDefine** out, int* out_count, int* out_cap) {
    if (!s) return;
    const char* eq = strchr(s, '=');
    char* name = 0;
    char* value = 0;

    if (eq) {
        name = (char*)malloc((size_t)(eq - s) + 1);
        if (!name) exit(1);
        memcpy(name, s, (size_t)(eq - s));
        name[eq - s] = 0;
        value = strdup(eq + 1);
    } else {
        name = strdup(s);
        value = strdup("1");
    }

    if (!name || !name[0]) {
        if (name) free(name);
        if (value) free(value);
        return;
    }

    if (*out_count == *out_cap) {
        int ncap = (*out_cap == 0) ? 8 : (*out_cap * 2);
        SccPPDefine* nd = (SccPPDefine*)realloc(*out, (size_t)ncap * sizeof(SccPPDefine));
        if (!nd) exit(1);
        *out = nd;
        *out_cap = ncap;
    }

    (*out)[(*out_count)++] = (SccPPDefine){name, value};
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("SCC v0.2\nUsage: scc [opts] -o out.o input.c\n       scc [opts] input.c out.o\n\nopts:\n  -I <dir>    add include search path\n  -D<name>=<value> define object-like macro\n  -D<name>    define object-like macro as 1\n");
        return 1;
    }

    const char* in_path = 0;
    const char* out_path = 0;

    char** include_paths = 0;
    int include_count = 0;
    int include_cap = 0;

    SccPPDefine* defines = 0;
    int define_count = 0;
    int define_cap = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                printf("Missing value after -o\n");
                return 1;
            }
            out_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-I") == 0) {
            if (i + 1 >= argc) {
                printf("Missing value after -I\n");
                return 1;
            }
            scc_opt_push_cstr(argv[++i], &include_paths, &include_count, &include_cap);
            continue;
        }

        if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2]) {
            scc_opt_push_cstr(argv[i] + 2, &include_paths, &include_count, &include_cap);
            continue;
        }

        if (strcmp(argv[i], "-D") == 0) {
            if (i + 1 >= argc) {
                printf("Missing value after -D\n");
                return 1;
            }
            scc_opt_push_define(argv[++i], &defines, &define_count, &define_cap);
            continue;
        }

        if (strncmp(argv[i], "-D", 2) == 0 && argv[i][2]) {
            scc_opt_push_define(argv[i] + 2, &defines, &define_count, &define_cap);
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

    SccPPConfig pp_cfg;
    memset(&pp_cfg, 0, sizeof(pp_cfg));
    pp_cfg.include_paths = (const char**)include_paths;
    pp_cfg.include_path_count = include_count;
    pp_cfg.defines = defines;
    pp_cfg.define_count = define_count;
    pp_cfg.max_include_depth = 64;

    scc_compile_file(in_path, out_path, &pp_cfg);

    if (include_paths) {
        for (int i = 0; i < include_count; i++) {
            if (include_paths[i]) free(include_paths[i]);
        }
        free(include_paths);
    }
    if (defines) {
        for (int i = 0; i < define_count; i++) {
            if (defines[i].name) free((void*)defines[i].name);
            if (defines[i].value) free((void*)defines[i].value);
        }
        free(defines);
    }

    printf("Success: %s\n", out_path);
    return 0;
}

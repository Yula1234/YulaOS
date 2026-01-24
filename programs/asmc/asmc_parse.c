// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_parse.h"

#include "asmc_buffer.h"
#include "asmc_expr.h"
#include "asmc_symbols.h"
#include "asmc_x86.h"

static int tokenize_line(char* line, char** tokens, int max_tokens) {
    int count = 0;
    char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r') *p++ = 0;
        if (*p == 0 || *p == ';') break;
        char* start = p;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else if (*p == '\'') {
            p++;
            while (*p && *p != '\'') p++;
            if (*p == '\'') p++;
        } else if (*p == '[') {
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' && *p != '\r') p++;
        }
        tokens[count++] = start;
        if (count >= max_tokens) break;
    }
    return count;
}

static int handle_directive(AssemblerCtx* ctx, char* cmd_name, char** tokens, int count) {
    if (strcmp(cmd_name, "format") == 0) {
        if (count < 2) panic(ctx, "format requires argument");
        if (strcmp(tokens[1], "binary") == 0) ctx->format = FMT_BIN;
        else if (strcmp(tokens[1], "elf") == 0) ctx->format = FMT_ELF;
        else panic(ctx, "Unknown format");
        return 1;
    }
    if (strcmp(cmd_name, "use16") == 0) {
        ctx->default_size = 2;
        ctx->code16 = 1;
        return 1;
    }
    if (strcmp(cmd_name, "use32") == 0) {
        ctx->default_size = 4;
        ctx->code16 = 0;
        return 1;
    }
    if (strcmp(cmd_name, "org") == 0) {
        if (ctx->format != FMT_BIN) panic(ctx, "org only valid in binary format");
        if (count < 2) panic(ctx, "org requires argument");
        if (ctx->pass == 1) {
            ctx->org = (uint32_t)eval_number(ctx, tokens[1]);
            ctx->has_org = 1;
        }
        return 1;
    }
    if (strcmp(cmd_name, "section") == 0) {
        if (strcmp(tokens[1], ".text") == 0) ctx->cur_sec = SEC_TEXT;
        else if (strcmp(tokens[1], ".data") == 0) ctx->cur_sec = SEC_DATA;
        else if (strcmp(tokens[1], ".bss") == 0) ctx->cur_sec = SEC_BSS;
        return 1;
    }
    if (strcmp(cmd_name, "global") == 0) {
        if (ctx->pass == 1) { Symbol* s = sym_add(ctx, tokens[1]); s->bind = SYM_GLOBAL; }
        return 1;
    }
    if (strcmp(cmd_name, "extern") == 0) {
        if (ctx->pass == 1) { Symbol* s = sym_add(ctx, tokens[1]); s->bind = SYM_EXTERN; }
        return 1;
    }
    if (strcmp(cmd_name, "align") == 0) {
        int a = eval_number(ctx, tokens[1]);
        if (a <= 0) panic(ctx, "Invalid alignment");

        Buffer* b = get_cur_buffer(ctx);
        if (ctx->cur_sec == SEC_BSS) {
            uint32_t size = ctx->bss.size;
            uint32_t aligned = (size + (uint32_t)a - 1) & ~((uint32_t)a - 1);
            ctx->bss.size = aligned;
        } else {
            if (ctx->pass == 1) {
                uint32_t size = b->size;
                uint32_t aligned = (size + (uint32_t)a - 1) & ~((uint32_t)a - 1);
                b->size = aligned;
            } else {
                while (b->size % (uint32_t)a != 0) {
                    buf_push(b, 0);
                }
            }
        }
        return 1;
    }
    if (strcmp(cmd_name, "db") == 0) {
        Buffer* b = get_cur_buffer(ctx);
        for (int k = 1; k < count; k++) {
            if (tokens[k][0] == '"') {
                char* s = tokens[k] + 1;
                while (*s && *s != '"') {
                    if (ctx->pass == 2) buf_push(b, *s); else b->size++;
                    s++;
                }
            } else {
                if (ctx->pass == 2) buf_push(b, eval_number(ctx, tokens[k])); else b->size++;
            }
        }
        return 1;
    }
    if (strcmp(cmd_name, "dw") == 0) {
        Buffer* b = get_cur_buffer(ctx);
        for (int k = 1; k < count; k++) {
            if (ctx->pass == 2) {
                int val = eval_number(ctx, tokens[k]);
                buf_push(b, val & 0xFF);
                buf_push(b, (val >> 8) & 0xFF);
            } else {
                b->size += 2;
            }
        }
        return 1;
    }
    if (strcmp(cmd_name, "dd") == 0) {
        Buffer* b = get_cur_buffer(ctx);
        for (int k = 1; k < count; k++) {
            if (ctx->pass == 2) {
                if ((tokens[k][0] >= '0' && tokens[k][0] <= '9') || tokens[k][0] == '-')
                    buf_push_u32(b, eval_number(ctx, tokens[k]));
                else {
                    char full[64];
                    resolve_symbol_name(ctx, tokens[k], full, sizeof(full));

                    Symbol* s = sym_find(ctx, full);
                    if (s && s->section == SEC_ABS) {
                        buf_push_u32(b, s->value);
                    } else if (s) {
                        if (ctx->format == FMT_BIN) {
                            uint32_t addr = resolve_abs_addr(ctx, s);
                            buf_push_u32(b, addr);
                        } else {
                            emit_reloc(ctx, R_386_32, full, b->size); buf_push_u32(b, 0);
                        }
                    } else {
                        buf_push_u32(b, eval_number(ctx, tokens[k]));
                    }
                }
            } else {
                b->size += 4;
            }
        }
        return 1;
    }
    if (strcmp(cmd_name, "resb") == 0 || strcmp(cmd_name, "rb") == 0) {
        if (ctx->cur_sec != SEC_BSS) panic(ctx, "resb only in .bss");
        ctx->bss.size += eval_number(ctx, tokens[1]);
        return 1;
    }
    if (strcmp(cmd_name, "resw") == 0 || strcmp(cmd_name, "rw") == 0) {
        if (ctx->cur_sec != SEC_BSS) panic(ctx, "resw only in .bss");
        ctx->bss.size += eval_number(ctx, tokens[1]) * 2;
        return 1;
    }
    if (strcmp(cmd_name, "resd") == 0 || strcmp(cmd_name, "rd") == 0) {
        if (ctx->cur_sec != SEC_BSS) panic(ctx, "resd only in .bss");
        ctx->bss.size += eval_number(ctx, tokens[1]) * 4;
        return 1;
    }
    return 0;
}

void process_line(AssemblerCtx* ctx, char* line) {
    char* tokens[MAX_TOKENS];
    int count = tokenize_line(line, tokens, MAX_TOKENS);
    if(count == 0) return;

    int len = strlen(tokens[0]);
    if(tokens[0][len-1] == ':') {
        tokens[0][len-1] = 0;

        char full[64];
        normalize_symbol_name(ctx, tokens[0], full, sizeof(full));

        if (tokens[0][0] != '.') {
            size_t gl = strlen(tokens[0]);
            if (gl >= sizeof(ctx->current_scope)) gl = sizeof(ctx->current_scope) - 1;
            memcpy(ctx->current_scope, tokens[0], gl);
            ctx->current_scope[gl] = 0;
        }

        sym_define_label(ctx, full);
        for(int k=0; k<count-1; k++) tokens[k] = tokens[k+1];
        count--;
        if(count == 0) return;
    }

    char* cmd_name = tokens[0];
    int force_size = 0;
    char* clean_tokens[3];
    int c_idx = 0;

    if (count >= 3 && strcmp(tokens[1], "equ") == 0) {
        if (ctx->pass == 1) {
            char full[64];
            normalize_symbol_name(ctx, tokens[0], full, sizeof(full));
            Symbol* s = sym_add(ctx, full);
            s->value = eval_number(ctx, tokens[2]);
            s->section = SEC_ABS;
            s->bind = SYM_LOCAL;
        }
        return;
    }

    for (int i=1; i<count && c_idx < 2; i++) {
        if (strcmp(tokens[i], "byte") == 0) { force_size = 1; continue; }
        if (strcmp(tokens[i], "word") == 0) { force_size = 2; continue; }
        if (strcmp(tokens[i], "dword") == 0) { force_size = 4; continue; }
        if (strcmp(tokens[i], "ptr") == 0) { continue; }
        clean_tokens[c_idx++] = tokens[i];
    }

    if (strcmp(cmd_name, "movb") == 0) { cmd_name = "mov"; force_size = 1; }

    if (handle_directive(ctx, cmd_name, tokens, count)) return;

    Operand o1 = {0}, o2 = {0};
    if(c_idx > 0) parse_operand(ctx, clean_tokens[0], &o1);
    if(c_idx > 1) parse_operand(ctx, clean_tokens[1], &o2);
    assemble_instr(ctx, cmd_name, force_size, &o1, &o2);
}

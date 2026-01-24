// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_buffer.h"
#include "asmc_core.h"
#include "asmc_output.h"
#include "asmc_parse.h"
#include "asmc_symbols.h"
#include "asmc_x86.h"

static void assembler_run_pass(AssemblerCtx* ctx, char* src, int pass) {
    ctx->pass = pass;
    ctx->line_num = 0;

    if (pass == 2) {
        ctx->text.size = 0;
        ctx->data.size = 0;
        ctx->bss.size = 0;
    }

    int i = 0;
    char line[MAX_LINE_LEN];

    while (src[i]) {
        int j = 0;
        while (src[i] && src[i] != '\n') {
            if (src[i] != '\r' && j < (MAX_LINE_LEN - 1)) line[j++] = src[i];
            i++;
        }
        line[j] = 0;
        if (src[i] == '\n') i++;
        ctx->line_num++;
        process_line(ctx, line);
    }
}

static void assembler_free_resources(AssemblerCtx* ctx) {
    sym_table_free(ctx);
    buf_free(&ctx->text);
    buf_free(&ctx->data);
    buf_free(&ctx->bss);
    buf_free(&ctx->rel_text);
    buf_free(&ctx->rel_data);

    isa_free_index();
}

int main(int argc, char** argv) {
    if(argc < 3) { printf("ASMC v2.2.1\nUsage: asmc in.asm out.o\n"); return 1; }

    int fd = open(argv[1], 0);
    if(fd < 0) { printf("Cannot open input file\n"); return 1; }

    Buffer src_buf;
    buf_init(&src_buf, 4096);
    while (1) {
        char tmp[1024];
        int r = read(fd, tmp, sizeof(tmp));
        if (r < 0) { printf("Read error\n"); close(fd); return 1; }
        if (r == 0) break;
        buf_write(&src_buf, tmp, (uint32_t)r);
    }
    close(fd);
    buf_push(&src_buf, 0);
    char* src = (char*)src_buf.data;

    AssemblerCtx* ctx = malloc(sizeof(AssemblerCtx));
    if (!ctx) { printf("Out of memory for context\n"); buf_free(&src_buf); return 1; }
    memset(ctx, 0, sizeof(AssemblerCtx));
    ctx->format = FMT_ELF;
    ctx->default_size = 4;
    ctx->code16 = 0;

    isa_build_index();

    buf_init(&ctx->text, 4096); buf_init(&ctx->data, 4096);
    buf_init(&ctx->bss, 0);     buf_init(&ctx->rel_text, 1024);
    buf_init(&ctx->rel_data, 1024);

    assembler_run_pass(ctx, src, 1);

    int elf_idx = 1;
    for(int k=0; k<ctx->sym_count; k++) {
        if (ctx->symbols[k].section != SEC_ABS) {
            ctx->symbols[k].elf_idx = elf_idx++;
        } else {
            ctx->symbols[k].elf_idx = 0;
        }
    }

    if (ctx->format == FMT_BIN) {
        uint32_t base = ctx->has_org ? ctx->org : 0;
        ctx->text_base = base;
        ctx->data_base = ctx->text_base + ctx->text.size;
        ctx->bss_base  = ctx->data_base + ctx->data.size;
    } else {
        ctx->text_base = 0;
        ctx->data_base = 0;
        ctx->bss_base  = 0;
    }

    assembler_run_pass(ctx, src, 2);

    if (ctx->format == FMT_BIN) write_binary(ctx, argv[2]);
    else write_elf(ctx, argv[2]);
    printf("Success: %s (%d bytes code, %d bytes data)\n", argv[2], ctx->text.size, ctx->data.size);

    assembler_free_resources(ctx);

    free(ctx);
    buf_free(&src_buf);
    return 0;
}

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "asmc_output.h"

#include "asmc_buffer.h"
#include "asmc_symbols.h"

void write_elf(AssemblerCtx* ctx, const char* filename) {
    Buffer strtab; buf_init(&strtab, 512); buf_push(&strtab, 0);
    Buffer symtab; buf_init(&symtab, 1024);
    Elf32_Sym null_sym = {0}; buf_write(&symtab, &null_sym, sizeof(null_sym));

    for(int i=0; i<ctx->sym_count; i++) {
        Symbol* s = &ctx->symbols[i];
        if (s->section == SEC_ABS) continue;

        Elf32_Sym es;
        es.st_name = buf_add_string(&strtab, s->name);
        es.st_value = s->value;
        es.st_size = 0;
        es.st_other = 0;
        int bind = (s->bind == SYM_GLOBAL || s->bind == SYM_EXTERN) ? STB_GLOBAL : STB_LOCAL;
        int type = (s->section == SEC_TEXT) ? STT_FUNC : (s->section != SEC_NULL ? STT_OBJECT : STT_NOTYPE);
        es.st_info = ELF32_ST_INFO(bind, type);
        if (s->bind == SYM_EXTERN) es.st_shndx = SHN_UNDEF;
        else if (s->section == SEC_TEXT) es.st_shndx = 1;
        else if (s->section == SEC_DATA) es.st_shndx = 2;
        else if (s->section == SEC_BSS) es.st_shndx = 3;
        else es.st_shndx = SHN_UNDEF;
        buf_write(&symtab, &es, sizeof(es));
    }

    Buffer shstr; buf_init(&shstr, 256); buf_push(&shstr, 0);
    int n_txt = buf_add_string(&shstr, ".text");
    int n_dat = buf_add_string(&shstr, ".data");
    int n_bss = buf_add_string(&shstr, ".bss");
    int n_sym = buf_add_string(&shstr, ".symtab");
    int n_str = buf_add_string(&shstr, ".strtab");
    int n_shs = buf_add_string(&shstr, ".shstrtab");
    int n_rt  = buf_add_string(&shstr, ".rel.text");
    int n_rd  = buf_add_string(&shstr, ".rel.data");

    uint32_t offset = sizeof(Elf32_Ehdr);
    uint32_t off_txt = offset; offset += ctx->text.size;
    uint32_t off_dat = offset; offset += ctx->data.size;
    uint32_t off_bss = offset;
    uint32_t off_sym = offset; offset += symtab.size;
    uint32_t off_str = offset; offset += strtab.size;
    uint32_t off_shs = offset; offset += shstr.size;
    uint32_t off_rt  = offset; offset += ctx->rel_text.size;
    uint32_t off_rd  = offset; offset += ctx->rel_data.size;
    uint32_t off_shdr = offset;

    Elf32_Ehdr eh = {0};
    eh.e_ident[0]=0x7F; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
    eh.e_ident[4]=1; eh.e_ident[5]=1; eh.e_ident[6]=1;
    eh.e_type = ET_REL; eh.e_machine = EM_386; eh.e_version = 1;
    eh.e_shoff = off_shdr; eh.e_ehsize = sizeof(eh);
    eh.e_shentsize = sizeof(Elf32_Shdr); eh.e_shnum = 9; eh.e_shstrndx = 6;

    int fd = open(filename, 1);
    if(fd < 0) { printf("Error creating file\n"); return; }

    write(fd, &eh, sizeof(eh));
    if(ctx->text.size) write(fd, ctx->text.data, ctx->text.size);
    if(ctx->data.size) write(fd, ctx->data.data, ctx->data.size);
    write(fd, symtab.data, symtab.size);
    write(fd, strtab.data, strtab.size);
    write(fd, shstr.data, shstr.size);
    if(ctx->rel_text.size) write(fd, ctx->rel_text.data, ctx->rel_text.size);
    if(ctx->rel_data.size) write(fd, ctx->rel_data.data, ctx->rel_data.size);

    Elf32_Shdr sh[9] = {0};
    sh[1].sh_name=n_txt; sh[1].sh_type=SHT_PROGBITS; sh[1].sh_flags=SHF_ALLOC|SHF_EXECINSTR; sh[1].sh_offset=off_txt; sh[1].sh_size=ctx->text.size; sh[1].sh_addralign=4;
    sh[2].sh_name=n_dat; sh[2].sh_type=SHT_PROGBITS; sh[2].sh_flags=SHF_ALLOC|SHF_WRITE; sh[2].sh_offset=off_dat; sh[2].sh_size=ctx->data.size; sh[2].sh_addralign=4;
    sh[3].sh_name=n_bss; sh[3].sh_type=SHT_NOBITS; sh[3].sh_flags=SHF_ALLOC|SHF_WRITE; sh[3].sh_offset=off_bss; sh[3].sh_size=ctx->bss.size; sh[3].sh_addralign=4;
    sh[4].sh_name=n_sym; sh[4].sh_type=SHT_SYMTAB; sh[4].sh_offset=off_sym; sh[4].sh_size=symtab.size; sh[4].sh_link=5; sh[4].sh_entsize=sizeof(Elf32_Sym); sh[4].sh_addralign=4; sh[4].sh_info=ctx->sym_count;
    sh[5].sh_name=n_str; sh[5].sh_type=SHT_STRTAB; sh[5].sh_offset=off_str; sh[5].sh_size=strtab.size; sh[5].sh_addralign=1;
    sh[6].sh_name=n_shs; sh[6].sh_type=SHT_STRTAB; sh[6].sh_offset=off_shs; sh[6].sh_size=shstr.size; sh[6].sh_addralign=1;
    sh[7].sh_name=n_rt; sh[7].sh_type=SHT_REL; sh[7].sh_offset=off_rt; sh[7].sh_size=ctx->rel_text.size; sh[7].sh_link=4; sh[7].sh_info=1; sh[7].sh_entsize=sizeof(Elf32_Rel); sh[7].sh_addralign=4;
    sh[8].sh_name=n_rd; sh[8].sh_type=SHT_REL; sh[8].sh_offset=off_rd; sh[8].sh_size=ctx->rel_data.size; sh[8].sh_link=4; sh[8].sh_info=2; sh[8].sh_entsize=sizeof(Elf32_Rel); sh[8].sh_addralign=4;

    write(fd, sh, sizeof(sh));
    close(fd);

    buf_free(&strtab); buf_free(&symtab); buf_free(&shstr);
}

void write_binary(AssemblerCtx* ctx, const char* filename) {
    int fd = open(filename, 1);
    if (fd < 0) { printf("Error creating file\n"); return; }

    if (ctx->text.size) write(fd, ctx->text.data, ctx->text.size);
    if (ctx->data.size) write(fd, ctx->data.data, ctx->data.size);

    close(fd);
}

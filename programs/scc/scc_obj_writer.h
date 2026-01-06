#ifndef SCC_OBJ_WRITER_H_INCLUDED
#define SCC_OBJ_WRITER_H_INCLUDED

#include "scc_core.h"
#include "scc_buffer.h"
#include "scc_elf.h"

static void write_elf_object(const char* out_path, Buffer* text, Buffer* data, uint32_t bss_size, Buffer* rel_text, Buffer* rel_data, SymTable* syms) {
    Buffer strtab; buf_init(&strtab, 128);
    buf_push_u8(&strtab, 0);

    Buffer symtab; buf_init(&symtab, 128);
    Elf32_Sym null_sym;
    memset(&null_sym, 0, sizeof(null_sym));
    buf_write(&symtab, &null_sym, sizeof(null_sym));

    for (int i = 0; i < syms->count; i++) {
        Symbol* ss = syms->data[i];
        if (!ss) continue;
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
        Symbol* s = syms->data[i];
        if (s && s->bind == STB_LOCAL) local_count++;
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

#endif

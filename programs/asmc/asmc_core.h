// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ASMC_CORE_H_INCLUDED
#define ASMC_CORE_H_INCLUDED

#include <yula.h>

#define MAX_LINE_LEN   1024
#define MAX_TOKEN_LEN   64
#define MAX_TOKENS      256
#define MAX_SYMBOLS    2048

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
#define STT_SECTION 3
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define R_386_32 1
#define R_386_PC32 2
#define ELF32_ST_INFO(b,t) (((b)<<4)+((t)&0xf))
#define ELF32_R_INFO(s,t)  (((s)<<8)+(unsigned char)(t))

typedef struct {
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
} Buffer;

typedef enum { SEC_NULL=0, SEC_TEXT, SEC_DATA, SEC_BSS, SEC_ABS } SectionID;
typedef enum { SYM_UNDEF, SYM_LOCAL, SYM_GLOBAL, SYM_EXTERN } SymBind;
typedef enum { FMT_ELF=0, FMT_BIN } OutputFormat;

typedef struct {
    char name[64];
    SymBind bind;
    SectionID section;
    uint32_t value;
    uint32_t elf_idx;
} Symbol;

typedef struct {
    int pass;
    int line_num;
    SectionID cur_sec;

    Buffer text;
    Buffer data;
    Buffer bss;
    Buffer rel_text;
    Buffer rel_data;

    Symbol* symbols;
    int sym_count;
    int sym_capacity;
    int* sym_hash;
    int sym_hash_size;
    char current_scope[64];
    OutputFormat format;
    int default_size;
    int code16;
    uint32_t text_base;
    uint32_t data_base;
    uint32_t bss_base;
    uint32_t org;
    int has_org;
} AssemblerCtx;

void panic(AssemblerCtx* ctx, const char* msg);

#endif

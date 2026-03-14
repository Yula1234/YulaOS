// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <stdint.h>

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
    Elf32_Word    p_type;        
    Elf32_Off     p_offset;      
    Elf32_Addr    p_vaddr;       
    Elf32_Addr    p_paddr;       
    Elf32_Word    p_filesz;      
    Elf32_Word    p_memsz;       
    Elf32_Word    p_flags;       
    Elf32_Word    p_align;       
} __attribute__((packed)) Elf32_Phdr;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t st_info;
    uint8_t st_other;
    Elf32_Half st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

#define SHT_SYMTAB 2u
#define SHN_UNDEF 0u

#endif
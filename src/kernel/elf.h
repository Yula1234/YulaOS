#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <stdint.h>

#define ELF_MAGIC 0x464C457F // "\x7FELF" Little Endian

typedef struct {
    uint32_t magic;
    uint8_t  elf[12];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;  
    uint32_t phoff; 
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf_header_t;

typedef struct {
    uint32_t type;     // PT_LOAD (1)
    uint32_t offset; 
    uint32_t vaddr; 
    uint32_t paddr;
    uint32_t filesz; 
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed)) elf_phdr_t;

#endif
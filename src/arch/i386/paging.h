// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

#define PTE_PRESENT 0x001u
#define PTE_RW      0x002u
#define PTE_USER    0x004u
#define PTE_PWT     0x008u
#define PTE_PCD     0x010u
#define PTE_PAT     0x080u

void paging_init(uint32_t ram_size_bytes);
void paging_init_pat(void);
int paging_pat_is_supported(void);

void paging_switch(uint32_t* dir_phys);
uint32_t* paging_get_dir(void);
uint32_t* paging_clone_directory(void); 
void paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);
int paging_is_user_accessible(uint32_t* dir, uint32_t virt);
uint32_t paging_get_phys(uint32_t* dir, uint32_t virt);

extern uint32_t* kernel_page_directory;

#endif
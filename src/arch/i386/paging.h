// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTE_PRESENT 0x001u
#define PTE_RW      0x002u
#define PTE_USER    0x004u
#define PTE_PWT     0x008u
#define PTE_PCD     0x010u
#define PTE_PAT     0x080u

void paging_init(uint32_t ram_size_bytes);
void paging_init_pat(void);
int paging_pat_is_supported(void);
void paging_init_mtrr_wc(uint32_t phys_base, uint32_t size);

void paging_switch(uint32_t* dir_phys);
uint32_t* paging_get_dir(void);
uint32_t* paging_clone_directory(void); 
void paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);
void paging_zero_phys_page(uint32_t phys);
int paging_is_user_accessible(uint32_t* dir, uint32_t virt);
uint32_t paging_get_phys(uint32_t* dir, uint32_t virt);

uint32_t paging_read_pde(uint32_t* dir, uint32_t pd_idx);
void paging_write_pde(uint32_t* dir, uint32_t pd_idx, uint32_t pde);

uint32_t paging_read_pt_entry(uint32_t pt_phys, uint32_t pt_idx);
void paging_write_pt_entry(uint32_t pt_phys, uint32_t pt_idx, uint32_t pte);

int paging_get_present_pte_safe(uint32_t* dir, uint32_t virt, uint32_t* out_pte);

uint32_t paging_fixmap_map(uint32_t phys);
void paging_fixmap_unmap(uint32_t virt);

extern uint32_t* kernel_page_directory;

#ifdef __cplusplus
}
#endif

#endif
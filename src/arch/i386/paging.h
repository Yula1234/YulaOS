/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

/*
 * i386 paging.
 *
 * This interface is the low-level backend used to build and manipulate page
 * directories/tables in 32-bit mode.
 *
 * The code intentionally keeps the contract small:
 *  - the kernel directory is a flat 1:1 map of RAM at boot time
 *  - processes clone the kernel half by copying PDEs
 *  - map/unmap operations are responsible for local TLB invalidation, while
 *    global kernel mappings additionally require cross-CPU invalidation
 *
 * Higher layers (proc/vmm/heap) build policy on top of this.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PTE flag bits used by this kernel.
 *
 * On i386, PTE/PDE low 12 bits are flags; upper bits are the aligned physical
 * address.
 */
#define PTE_PRESENT 0x001u
#define PTE_RW      0x002u
#define PTE_USER    0x004u
#define PTE_PWT     0x008u
#define PTE_PCD     0x010u
#define PTE_PAT     0x080u
#define PTE_SUPER   0x080u
#define PTE_GLOBAL  0x100u

/* paging_map_ex() flags. */
#define PAGING_MAP_NO_TLB_FLUSH 0x00000001u

/* Build identity+kernel mappings and switch to the initial kernel directory. */
void paging_init(uint32_t ram_size_bytes);

/* Configure PAT to make a WC memory type available for selected mappings. */
void paging_init_pat(void);

/* Query CPU support for PAT. The result is cached after the first call. */
int paging_pat_is_supported(void);

/*
 * Program one variable MTRR as WC for a physical range.
 *
 * This is used for framebuffer write-combining on CPUs that support MTRRs.
 */
void paging_init_mtrr_wc(uint32_t phys_base, uint32_t size);

/* Load CR3 with the provided page directory physical address. */
void paging_switch(uint32_t* dir_phys);

/* Return the currently active CR3 value. */
uint32_t* paging_get_dir(void);

/*
 * Create a new directory that shares the kernel half with the global kernel
 * directory.
 */
uint32_t* paging_clone_directory(void); 

/*
 * Map a 4KiB page.
 *
 * `dir` points to a page directory (physical address, identity-mapped).
 * `virt` and `phys` are 4KiB-aligned.
 * `flags` are the PTE flags (PTE_PRESENT is expected for valid mappings).
 */
void paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);

void paging_map_batch(
    uint32_t* dir, uint32_t virt_start,
    const uint32_t* phys_array, uint32_t count,
    uint32_t flags, uint32_t map_flags
);

void paging_map_ex(
    uint32_t* dir, uint32_t virt,
    uint32_t phys, uint32_t flags,
    uint32_t map_flags
);

void paging_map_4m(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);

typedef int (*paging_unmap_visitor_t)(uint32_t virt, uint32_t pte, void* ctx);

void paging_unmap_range_ex(
    uint32_t* dir, uint32_t start_vaddr, uint32_t end_vaddr,
    paging_unmap_visitor_t visitor, void* visitor_ctx
);

void paging_unmap_range(uint32_t* dir, uint32_t start_vaddr, uint32_t end_vaddr);

void paging_unmap_range_no_tlb(
    uint32_t* dir, uint32_t start_vaddr, uint32_t end_vaddr,
    paging_unmap_visitor_t visitor, void* visitor_ctx
);

/* Zero a physical page, using a temporary fixmap mapping once paging is on. */
void paging_zero_phys_page(uint32_t phys);

/* Check that a virtual address resolves to a user-accessible mapping. */
int paging_is_user_accessible(uint32_t* dir, uint32_t virt);

/* Translate a virtual address to a physical address. Returns 0 if unmapped. */
uint32_t paging_get_phys(uint32_t* dir, uint32_t virt);

/*
 * Get the PTE for a virtual address if present.
 * Returns 1 and stores PTE in *out_pte if the mapping exists, 0 otherwise.
 */
int paging_get_present_pte(uint32_t* dir, uint32_t virt, uint32_t* out_pte);

/*
 * Global kernel page directory.
 *
 * This is the directory installed during early boot and used as the reference
 * template for per-process directories. It is also the directory referenced by
 * the fixmap logic.
 */
extern uint32_t* kernel_page_directory;

int paging_register_dir_lock(uint32_t* dir, void* lock);
void paging_unregister_dir_lock(uint32_t* dir);

#ifdef __cplusplus
}
#endif

#endif
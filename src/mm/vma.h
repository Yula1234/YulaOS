/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef MM_VMA_H
#define MM_VMA_H

#include <lib/maple_tree.h>

#include <kernel/rcu.h>

#include <stdint.h>
#include <stddef.h>

struct vfs_node;

struct proc_mem;

#define VMA_MAP_SHARED  1u
#define VMA_MAP_PRIVATE 2u
#define VMA_MAP_STACK   4u

typedef struct vma_region vma_region_t;

struct vma_region {
    uint32_t vaddr_start;
    uint32_t vaddr_end;

    uint32_t file_offset;
    uint32_t length;
    uint32_t file_size;

    uint32_t map_flags;

    struct vfs_node* file;

    rcu_head_t rcu;
};

#ifdef __cplusplus
extern "C" {
#endif

void vma_init(struct proc_mem* mem);

void vma_destroy(struct proc_mem* mem);

vma_region_t* vma_create(
    struct proc_mem* mem, uint32_t vaddr, uint32_t size, struct vfs_node* file,
    uint32_t file_offset,uint32_t file_size, uint32_t flags
);

vma_region_t* vma_find(struct proc_mem* mem, uint32_t vaddr);
vma_region_t* vma_find_overlap(struct proc_mem* mem, uint32_t start, uint32_t end_excl);

int vma_has_overlap(struct proc_mem* mem, uint32_t start, uint32_t end_excl);

int vma_remove(struct proc_mem* mem, uint32_t vaddr, uint32_t len);

int vma_validate_range(struct proc_mem* mem, uint32_t start, uint32_t end_excl);

uint32_t vma_alloc_slot(struct proc_mem* mem, uint32_t size,uint32_t* out_vaddr);

#ifdef __cplusplus
}
#endif

#endif

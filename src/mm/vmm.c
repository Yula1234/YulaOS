// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <hal/lock.h>
#include <arch/i386/paging.h>
#include <mm/pmm.h>
#include <drivers/vga.h>
#include <kernel/panic.h>
#include <lib/rbtree.h>

#include "vmm.h"

typedef struct vm_free_block {
    struct rb_node node;
    uint32_t start;
    uint32_t size;
} vm_free_block_t;

#define MAX_VMM_NODES 4096

static vm_free_block_t node_pool[MAX_VMM_NODES];
static struct rb_root vmm_root = RB_ROOT;
static spinlock_t vmm_lock;
static size_t vmm_used_pages_count = 0;

extern uint32_t* kernel_page_directory;

static int last_idx_allocated = 0;

static vm_free_block_t* alloc_node(void) {
    for (int i = last_idx_allocated; i < MAX_VMM_NODES; i++) {
        if (node_pool[i].size == 0 && node_pool[i].start == 0) {
            last_idx_allocated = i + 1;
            return &node_pool[i];
        }
    }

    for (int i = 0; i < last_idx_allocated; i++) {
        if (node_pool[i].size == 0 && node_pool[i].start == 0) {
            last_idx_allocated = i + 1;
            return &node_pool[i];
        }
    }

    return 0;
}

static void free_node(vm_free_block_t* node) {
    memset(node, 0, sizeof(*node));
}

static void insert_block(vm_free_block_t* block) {
    struct rb_node **new_node = &(vmm_root.rb_node), *parent = 0;

    while (*new_node) {
        vm_free_block_t *this_block = rb_entry(*new_node, vm_free_block_t, node);
        parent = *new_node;
        if (block->start < this_block->start)
            new_node = &((*new_node)->rb_left);
        else if (block->start > this_block->start)
            new_node = &((*new_node)->rb_right);
        else
            return;
    }

    rb_link_node(&block->node, parent, new_node);
    rb_insert_color(&block->node, &vmm_root);
}

void vmm_init(void) {
    spinlock_init(&vmm_lock);
    memset(node_pool, 0, sizeof(node_pool));
    vmm_root = RB_ROOT;
    vmm_used_pages_count = 0;

    vm_free_block_t* initial_block = alloc_node();
    if (initial_block) {
        initial_block->start = 0xC0000000; 
        initial_block->size = 0x40000000;
        insert_block(initial_block);
    }
}

void* vmm_alloc_pages(size_t count) {
    uint32_t flags = spinlock_acquire_safe(&vmm_lock);
    
    struct rb_node *node = rb_first(&vmm_root);
    while (node) {
        vm_free_block_t *block = rb_entry(node, vm_free_block_t, node);
        
        if (block->size >= count * 4096) {
            uint32_t addr = block->start;
            
            if (block->size == count * 4096) {
                rb_erase(&block->node, &vmm_root);
                free_node(block);
            } else {
                block->start += count * 4096;
                block->size -= count * 4096;
            }
            
            vmm_used_pages_count += count;
            spinlock_release_safe(&vmm_lock, flags);
            
            for (size_t i = 0; i < count; i++) {
                uint32_t phys = (uint32_t)pmm_alloc_block();
                if (!phys) {
                    return 0; 
                }
                paging_map(kernel_page_directory, addr + i * 4096, phys, PTE_PRESENT | PTE_RW);
            }
            
            return (void*)addr;
        }
        
        node = rb_next(node);
    }

    spinlock_release_safe(&vmm_lock, flags);
    return 0;
}

void vmm_free_pages(void* ptr, size_t count) {
    if (!ptr || count == 0) return;
    
    uint32_t addr = (uint32_t)ptr;
    uint32_t flags = spinlock_acquire_safe(&vmm_lock);

    for (size_t i = 0; i < count; i++) {
        uint32_t phys = paging_get_phys(kernel_page_directory, addr + i * 4096);
        if (phys) {
            pmm_free_block((void*)phys);
        }
        paging_map(kernel_page_directory, addr + i * 4096, 0, 0); // Unmap
    }
    
    vm_free_block_t* new_block = alloc_node();
    if (!new_block) {
        panic("VMM: Out of nodes!");
    }
    
    new_block->start = addr;
    new_block->size = count * 4096;
    insert_block(new_block);

    struct rb_node *node = rb_first(&vmm_root);
    while (node) {
        vm_free_block_t *block = rb_entry(node, vm_free_block_t, node);
        struct rb_node *next_node = rb_next(node);
        
        if (next_node) {
            vm_free_block_t *next_block = rb_entry(next_node, vm_free_block_t, node);
            if (block->start + block->size == next_block->start) {
                block->size += next_block->size;
                rb_erase(&next_block->node, &vmm_root);
                free_node(next_block);
                continue; 
            }
        }
        node = next_node;
    }
    
    vmm_used_pages_count -= count;
    spinlock_release_safe(&vmm_lock, flags);
}

size_t vmm_get_used_pages(void) {
    return vmm_used_pages_count;
}

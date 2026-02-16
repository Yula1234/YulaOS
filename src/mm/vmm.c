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
    struct rb_node node_addr;
    struct rb_node node_size;
    uint32_t start;
    uint32_t size;
    struct vm_free_block* next_free;
} vm_free_block_t;

#define MAX_VMM_NODES 4096

static vm_free_block_t node_pool[MAX_VMM_NODES];
static vm_free_block_t* free_nodes_head = 0;

static struct rb_root vmm_root_addr = RB_ROOT;
static struct rb_root vmm_root_size = RB_ROOT;

static spinlock_t vmm_lock;
static size_t vmm_used_pages_count = 0;

extern uint32_t* kernel_page_directory;

static void init_node_pool(void) {
    for (int i = 0; i < MAX_VMM_NODES - 1; i++) {
        node_pool[i].next_free = &node_pool[i + 1];
    }
    node_pool[MAX_VMM_NODES - 1].next_free = 0;
    free_nodes_head = &node_pool[0];
}

static vm_free_block_t* alloc_node(void) {
    if (!free_nodes_head) return 0;
    vm_free_block_t* node = free_nodes_head;
    free_nodes_head = node->next_free;
    memset(node, 0, sizeof(*node));
    return node;
}

static void free_node(vm_free_block_t* node) {
    node->next_free = free_nodes_head;
    free_nodes_head = node;
}

static void insert_block_addr(vm_free_block_t* block) {
    struct rb_node **new_node = &(vmm_root_addr.rb_node), *parent = 0;

    while (*new_node) {
        vm_free_block_t *this_block = rb_entry(*new_node, vm_free_block_t, node_addr);
        parent = *new_node;
        if (block->start < this_block->start)
            new_node = &((*new_node)->rb_left);
        else if (block->start > this_block->start)
            new_node = &((*new_node)->rb_right);
        else
            return;
    }

    rb_link_node(&block->node_addr, parent, new_node);
    rb_insert_color(&block->node_addr, &vmm_root_addr);
}

static void insert_block_size(vm_free_block_t* block) {
    struct rb_node **new_node = &(vmm_root_size.rb_node), *parent = 0;

    while (*new_node) {
        vm_free_block_t *this_block = rb_entry(*new_node, vm_free_block_t, node_size);
        parent = *new_node;
        
        if (block->size < this_block->size)
            new_node = &((*new_node)->rb_left);
        else if (block->size > this_block->size)
            new_node = &((*new_node)->rb_right);
        else {
            if (block->start < this_block->start)
                new_node = &((*new_node)->rb_left);
            else
                new_node = &((*new_node)->rb_right);
        }
    }

    rb_link_node(&block->node_size, parent, new_node);
    rb_insert_color(&block->node_size, &vmm_root_size);
}

static vm_free_block_t* find_best_fit(uint32_t size) {
    struct rb_node *node = vmm_root_size.rb_node;
    vm_free_block_t *best_fit = 0;

    while (node) {
        vm_free_block_t *block = rb_entry(node, vm_free_block_t, node_size);

        if (block->size >= size) {
            best_fit = block;
            node = node->rb_left;
        } else {
            node = node->rb_right;
        }
    }
    return best_fit;
}

void vmm_init(void) {
    spinlock_init(&vmm_lock);
    init_node_pool();
    vmm_root_addr = RB_ROOT;
    vmm_root_size = RB_ROOT;
    vmm_used_pages_count = 0;

    vm_free_block_t* initial_block = alloc_node();
    if (initial_block) {
        initial_block->start = 0xC0000000; 
        initial_block->size = 0x40000000;
        insert_block_addr(initial_block);
        insert_block_size(initial_block);
    }
}

void* vmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    uint32_t size_bytes = count * 4096;
    
    uint32_t flags = spinlock_acquire_safe(&vmm_lock);
    
    vm_free_block_t *block = find_best_fit(size_bytes);
    
    if (!block) {
        spinlock_release_safe(&vmm_lock, flags);
        return 0;
    }

    uint32_t addr = block->start;

    rb_erase(&block->node_addr, &vmm_root_addr);
    rb_erase(&block->node_size, &vmm_root_size);

    if (block->size == size_bytes) {
        free_node(block);
    } else {
        block->start += size_bytes;
        block->size -= size_bytes;
        
        insert_block_addr(block);
        insert_block_size(block);
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

void vmm_free_pages(void* ptr, size_t count) {
    if (!ptr || count == 0) return;
    
    uint32_t addr = (uint32_t)ptr;
    uint32_t size_bytes = count * 4096;
    
    uint32_t flags = spinlock_acquire_safe(&vmm_lock);

    for (size_t i = 0; i < count; i++) {
        uint32_t phys = paging_get_phys(kernel_page_directory, addr + i * 4096);
        if (phys) {
            pmm_free_block((void*)phys);
        }
        paging_map(kernel_page_directory, addr + i * 4096, 0, 0); 
    }
    
    vm_free_block_t* new_block = alloc_node();
    if (!new_block) {
        spinlock_release_safe(&vmm_lock, flags);
        panic("VMM: Out of metadata nodes during free!");
        return;
    }
    
    new_block->start = addr;
    new_block->size = size_bytes;

    insert_block_addr(new_block);

    struct rb_node *prev_node = rb_prev(&new_block->node_addr);
    struct rb_node *next_node = rb_next(&new_block->node_addr);

    if (next_node) {
        vm_free_block_t *next_block = rb_entry(next_node, vm_free_block_t, node_addr);
        if (new_block->start + new_block->size == next_block->start) {
            new_block->size += next_block->size;
            
            rb_erase(&next_block->node_addr, &vmm_root_addr);
            rb_erase(&next_block->node_size, &vmm_root_size);
            free_node(next_block);
        }
    }

    if (prev_node) {
        vm_free_block_t *prev_block = rb_entry(prev_node, vm_free_block_t, node_addr);
        if (prev_block->start + prev_block->size == new_block->start) {
            rb_erase(&prev_block->node_size, &vmm_root_size);
            
            prev_block->size += new_block->size;
            
            rb_erase(&new_block->node_addr, &vmm_root_addr);
            free_node(new_block);
            
            new_block = prev_block;
        }
    }

    insert_block_size(new_block);
    
    vmm_used_pages_count -= count;
    spinlock_release_safe(&vmm_lock, flags);
}

size_t vmm_get_used_pages(void) {
    return vmm_used_pages_count;
}

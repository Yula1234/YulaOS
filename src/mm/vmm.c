// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <hal/lock.h>
#include <arch/i386/paging.h>
#include <mm/pmm.h>
#include <drivers/vga.h>

#include "vmm.h"

typedef enum { RED, BLACK } rb_color_t;

typedef struct vm_free_block {
    uint32_t start;     
    uint32_t size;      
    uint32_t max_size;
    
    rb_color_t color;
    struct vm_free_block *left, *right, *parent;
    
    int is_active;   
} vm_free_block_t;

#define MAX_VMM_NODES 4096

static vm_free_block_t node_pool[MAX_VMM_NODES];
static vm_free_block_t* root = 0;
static spinlock_t vmm_lock;
static size_t vmm_used_pages_count = 0;

extern uint32_t* kernel_page_directory;

static int last_idx_allocated = 0;

static vm_free_block_t* alloc_node(void) {
    for (int i = last_idx_allocated; i < MAX_VMM_NODES; i++) {
        if (!node_pool[i].is_active) {
            last_idx_allocated = i + 1;
            node_pool[i].is_active = 1;
            node_pool[i].left = node_pool[i].right = node_pool[i].parent = 0;
            node_pool[i].color = RED;
            node_pool[i].max_size = 0;
            return &node_pool[i];
        }
    }

    for (int i = 0; i < last_idx_allocated; i++) {
        if (!node_pool[i].is_active) {
            last_idx_allocated = i + 1;
            node_pool[i].is_active = 1;
            node_pool[i].left = node_pool[i].right = node_pool[i].parent = 0;
            node_pool[i].color = RED;
            node_pool[i].max_size = 0;
            return &node_pool[i];
        }
    }

    return 0;
}
static void free_node(vm_free_block_t* node) {
    node->is_active = 0;
}

static inline uint32_t get_max_size(vm_free_block_t* node) {
    if (!node) return 0;
    return node->max_size;
}

static void update_max_size(vm_free_block_t* node) {
    if (!node) return;
    uint32_t left_max = get_max_size(node->left);
    uint32_t right_max = get_max_size(node->right);
    uint32_t max = node->size;
    if (left_max > max) max = left_max;
    if (right_max > max) max = right_max;
    node->max_size = max;
}

static void update_max_size_upward(vm_free_block_t* node) {
    while (node) {
        update_max_size(node);
        node = node->parent;
    }
}

static void rotate_left(vm_free_block_t* x) {
    vm_free_block_t* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent) root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
    
    update_max_size(x);
    update_max_size(y);
}

static void rotate_right(vm_free_block_t* x) {
    vm_free_block_t* y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (!x->parent) root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
    
    update_max_size(x);
    update_max_size(y);
}

static void insert_fixup(vm_free_block_t* z) {
    while (z->parent && z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            vm_free_block_t* y = z->parent->parent->right;
            if (y && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotate_right(z->parent->parent);
            }
        } else {
            vm_free_block_t* y = z->parent->parent->left;
            if (y && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotate_left(z->parent->parent);
            }
        }
    }
    root->color = BLACK;
}

static void rb_insert(vm_free_block_t* z) {
    vm_free_block_t* y = 0;
    vm_free_block_t* x = root;
    while (x) {
        y = x;
        if (z->start < x->start) x = x->left;
        else x = x->right;
    }
    z->parent = y;
    if (!y) root = z;
    else if (z->start < y->start) y->left = z;
    else y->right = z;
    
    z->left = z->right = 0;
    z->color = RED;
    z->max_size = z->size;
    update_max_size_upward(z);
    insert_fixup(z);
}

static void rb_transplant(vm_free_block_t* u, vm_free_block_t* v) {
    if (!u->parent) root = v;
    else if (u == u->parent->left) u->parent->left = v;
    else u->parent->right = v;
    if (v) v->parent = u->parent;
}

static vm_free_block_t* tree_minimum(vm_free_block_t* x) {
    while (x->left) x = x->left;
    return x;
}

static void delete_fixup(vm_free_block_t* x) {
    while (x != root && (!x || x->color == BLACK)) {
        if (!x || !x->parent) break;
        if (x == x->parent->left) {
            vm_free_block_t* w = x->parent->right;
            if (!w) break;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                rotate_left(x->parent);
                w = x->parent->right;
                if (!w) break;
            }
            if ((!w->left || w->left->color == BLACK) && (!w->right || w->right->color == BLACK)) {
                w->color = RED;
                update_max_size(x->parent);
                x = x->parent;
            } else {
                if (!w->right || w->right->color == BLACK) {
                    if (w->left) w->left->color = BLACK;
                    w->color = RED;
                    rotate_right(w);
                    w = x->parent->right;
                    if (!w) break;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                if (w->right) w->right->color = BLACK;
                rotate_left(x->parent);
                x = root;
            }
        } else {
            vm_free_block_t* w = x->parent->left;
            if (!w) break;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                rotate_right(x->parent);
                w = x->parent->left;
                if (!w) break;
            }
            if ((!w->right || w->right->color == BLACK) && (!w->left || w->left->color == BLACK)) {
                w->color = RED;
                update_max_size(x->parent);
                x = x->parent;
            } else {
                if (!w->left || w->left->color == BLACK) {
                    if (w->right) w->right->color = BLACK;
                    w->color = RED;
                    rotate_left(w);
                    w = x->parent->left;
                    if (!w) break;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                if (w->left) w->left->color = BLACK;
                rotate_right(x->parent);
                x = root;
            }
        }
    }
    if (x) {
        x->color = BLACK;
        update_max_size(x);
    }
}

static void rb_delete(vm_free_block_t* z) {
    vm_free_block_t* y = z;
    vm_free_block_t* x;
    vm_free_block_t* fixup_start = 0;
    rb_color_t y_original_color = y->color;

    if (!z->left) {
        x = z->right;
        fixup_start = z->parent;
        rb_transplant(z, z->right);
    } else if (!z->right) {
        x = z->left;
        fixup_start = z->parent;
        rb_transplant(z, z->left);
    } else {
        y = tree_minimum(z->right);
        y_original_color = y->color;
        x = y->right;
        fixup_start = y->parent;
        if (y->parent == z) {
            if (x) x->parent = y;
            fixup_start = y;
        } else {
            rb_transplant(y, y->right);
            y->right = z->right;
            if (y->right) y->right->parent = y;
        }
        rb_transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
        y->max_size = z->max_size;
    }
    if (y_original_color == BLACK && x) delete_fixup(x);
    if (fixup_start) update_max_size_upward(fixup_start);
    free_node(z);
}

void vmm_init(void) {
    spinlock_init(&vmm_lock);
    memset(node_pool, 0, sizeof(node_pool));
    root = 0;
    vmm_used_pages_count = 0;

    vm_free_block_t* initial_block = alloc_node();
    initial_block->start = KERNEL_HEAP_START;
    initial_block->size = KERNEL_HEAP_SIZE / PAGE_SIZE;
    initial_block->max_size = initial_block->size;
    
    rb_insert(initial_block);
}

static vm_free_block_t* find_fit_optimized(vm_free_block_t* node, size_t pages) {
    if (!node) return 0;
    
    if (node->max_size < pages) return 0;
    
    vm_free_block_t* best = 0;
    
    if (node->left && get_max_size(node->left) >= pages) {
        best = find_fit_optimized(node->left, pages);
        if (best) return best;
    }
    
    if (node->size >= pages) {
        if (!best || node->size < best->size) {
            best = node;
        }
    }
    
    if (node->right && get_max_size(node->right) >= pages) {
        vm_free_block_t* right_best = find_fit_optimized(node->right, pages);
        if (right_best && (!best || right_best->size < best->size)) {
            best = right_best;
        }
    }
    
    return best;
}

void* vmm_alloc_pages(size_t pages) {
    if (pages == 0) return 0;
    uint32_t flags = spinlock_acquire_safe(&vmm_lock);

    vm_free_block_t* node = find_fit_optimized(root, pages);
    if (!node) {
        spinlock_release_safe(&vmm_lock, flags);
        return 0; // OOM
    }
    
    uint32_t alloc_virt_addr = node->start + (node->size - pages) * PAGE_SIZE;
    node->size -= pages;
    update_max_size_upward(node);

    if (node->size == 0) {
        rb_delete(node);
    }

    spinlock_release_safe(&vmm_lock, flags);

    for (size_t i = 0; i < pages; i++) {
        void* phys = pmm_alloc_block();
        if (!phys) {
            return 0;
        }
        paging_map(kernel_page_directory,
                   alloc_virt_addr + i * PAGE_SIZE,
                   (uint32_t)phys,
                   3);
    }

    uint32_t flags2 = spinlock_acquire_safe(&vmm_lock);
    vmm_used_pages_count += pages;
    spinlock_release_safe(&vmm_lock, flags2);
    
    return (void*)alloc_virt_addr;
}

static vm_free_block_t* find_predecessor(uint32_t addr) {
    vm_free_block_t* current = root;
    vm_free_block_t* best = 0;
    
    while (current) {
        if (current->start < addr) {
            best = current;
            current = current->right;
        } else {
            current = current->left;
        }
    }
    return best;
}

static vm_free_block_t* find_successor(uint32_t addr) {
    vm_free_block_t* current = root;
    vm_free_block_t* best = 0;
    
    while (current) {
        if (current->start > addr) {
            best = current;
            current = current->left;
        } else {
            current = current->right;
        }
    }
    return best;
}

void vmm_free_pages(void* virt, size_t pages) {
    if (!virt || pages == 0) return;
    uint32_t vaddr = (uint32_t)virt;

    for (size_t i = 0; i < pages; i++) {
        uint32_t curr = vaddr + i * PAGE_SIZE;
        uint32_t phys = paging_get_phys(kernel_page_directory, curr);

        paging_map(kernel_page_directory, curr, 0, 0);

        if (phys) {
            pmm_free_block((void*)phys);
        }
    }

    uint32_t flags = spinlock_acquire_safe(&vmm_lock);

    vmm_used_pages_count -= pages;

    vm_free_block_t* prev = find_predecessor(vaddr);
    vm_free_block_t* next = find_successor(vaddr);
    
    int merged_prev = 0;
    int merged_next = 0;

    if (prev && (prev->start + prev->size * PAGE_SIZE == vaddr)) {
        prev->size += pages;
        vaddr = prev->start;
        pages = prev->size;
        merged_prev = 1;
        update_max_size_upward(prev);
    }

    if (next && (vaddr + pages * PAGE_SIZE == next->start)) {
        if (merged_prev) {
            prev->size += next->size;
            rb_delete(next);
            update_max_size_upward(prev);
        } else {
            size_t new_size = pages + next->size;
            rb_delete(next);
            
            vm_free_block_t* new_node = alloc_node();
            new_node->start = vaddr;
            new_node->size = new_size;
            new_node->max_size = new_size;
            rb_insert(new_node);
        }
        merged_next = 1;
    }

    if (!merged_prev && !merged_next) {
        vm_free_block_t* new_node = alloc_node();
        new_node->start = vaddr;
        new_node->size = pages;
        new_node->max_size = pages;
        rb_insert(new_node);
    }

    spinlock_release_safe(&vmm_lock, flags);
}

int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    paging_map(kernel_page_directory, virt, phys, flags);
    return 0;
}

size_t vmm_get_used_pages(void) {
    return vmm_used_pages_count;
}
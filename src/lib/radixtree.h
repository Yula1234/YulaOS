/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_RADIXTREE_H
#define LIB_RADIXTREE_H

#include <kernel/locking/spinlock.h>
#include <kernel/rcu.h>

#include <lib/compiler.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIX_TREE_MAP_SHIFT 6u
#define RADIX_TREE_MAP_SIZE  (1u << RADIX_TREE_MAP_SHIFT)
#define RADIX_TREE_MAP_MASK  (RADIX_TREE_MAP_SIZE - 1u)
#define RADIX_TREE_MAX_DEPTH ((32u + RADIX_TREE_MAP_SHIFT - 1u) / RADIX_TREE_MAP_SHIFT)

struct radix_node;
typedef struct radix_node radix_node_t;

struct radix_node {
    rcu_head_t rcu_;

    uint8_t shift_;
    uint8_t offset_;
    uint16_t count_;

    struct radix_node* parent_;

    rcu_ptr_t slots_[RADIX_TREE_MAP_SIZE];
};

typedef struct radix_tree {
    spinlock_t lock_;
    
    rcu_ptr_t root_;

    uint32_t max_depth_;
} radix_tree_t;

void radix_tree_init(radix_tree_t* tree);

void radix_tree_destroy(radix_tree_t* tree);

int radix_tree_insert(radix_tree_t* tree, uint32_t key, void* value);

void* radix_tree_remove(radix_tree_t* tree, uint32_t key);

void* radix_tree_lookup(radix_tree_t* tree, uint32_t key);

void* radix_tree_find_next(radix_tree_t* tree, uint32_t* inout_key);

#ifdef __cplusplus
}
#endif

#endif /* LIB_RADIXTREE_H */
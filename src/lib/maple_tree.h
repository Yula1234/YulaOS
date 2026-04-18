/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_MAPLE_TREE_H
#define LIB_MAPLE_TREE_H

#include <lib/compiler.h>

#include <kernel/rcu.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_SLOT_BITS   4u
#define MT_SLOT_COUNT  (1u << MT_SLOT_BITS)
#define MT_PIVOT_COUNT (MT_SLOT_COUNT - 1u)

#define MT_NODE_LEAF   0u
#define MT_NODE_RANGE  1u

typedef struct ma_node {
    uint32_t parent;
    union {
        struct {
            uint32_t pivots[MT_PIVOT_COUNT];
            void*    slots[MT_SLOT_COUNT];
        };

        struct {
            void*    leaf_slots[MT_SLOT_COUNT];
            uint32_t leaf_pivots[MT_PIVOT_COUNT];
        };
    };
} ma_node_t;

typedef struct ma_state {
    struct maple_tree* tree;
    
    uint32_t index;
    uint32_t last;
    
    ma_node_t* node;
    
    uint8_t offset;
    uint8_t depth;

    uint8_t alloc_cnt;
} ma_state_t;

typedef struct maple_tree {
    union {
        void*        ma_root;
        ma_node_t*   ma_root_node;
    };

    spinlock_t ma_lock;
    
    uint32_t   ma_flags;
} maple_tree_t;

#define MA_ROOT_FLAG ((uintptr_t)1u)
#define MT_FLAG_LOCK_NESTED (1u << 0u)

___inline void mt_init(maple_tree_t* mt) {
    mt->ma_root = (void*)MA_ROOT_FLAG;
    
    mt->ma_flags = 0u;
    
    spinlock_init(&mt->ma_lock);
}

___inline int mt_empty(const maple_tree_t* mt) {
    return mt->ma_root == (void*)MA_ROOT_FLAG;
}

___inline int mt_is_entry_ptr(const void* entry) {
    return ((uintptr_t)entry & 1u) != 0u && (uintptr_t)entry != MA_ROOT_FLAG;
}

___inline void* mt_slot_to_entry(void* ptr) {
    return (void*)((uintptr_t)ptr | 1u);
}

___inline void* mt_entry_to_slot(void* entry) {
    return (void*)((uintptr_t)entry & ~1u);
}

___inline int ma_node_is_leaf(const ma_node_t* node) {
    return (node->parent & 1u) == MT_NODE_LEAF;
}

___inline int ma_node_is_range(const ma_node_t* node) {
    return (node->parent & 1u) == MT_NODE_RANGE;
}

___inline ma_node_t* ma_parent(const ma_node_t* node) {
    return (ma_node_t*)(node->parent & ~0x1Fu);
}

___inline uint32_t ma_parent_slot(const ma_node_t* node) {
    return (node->parent >> 1u) & 0x0Fu;
}

___inline void ma_set_parent(ma_node_t* node, ma_node_t* parent, uint8_t slot) {
    uint32_t type_bit = node->parent & 1u;

    node->parent = ((uint32_t)parent & ~0x1Fu) | type_bit | (((uint32_t)slot & 0x0Fu) << 1u);
}

___inline void ma_set_type(ma_node_t* node, uint32_t type) {
    node->parent = (node->parent & ~1u) | (type & 1u);
}

___inline uint32_t ma_pivot(const ma_node_t* node, uint8_t idx) {
    if (idx >= MT_PIVOT_COUNT) {
        return 0xFFFFFFFFu;
    }

    return node->pivots[idx];
}

___inline void* ma_slot_rcu(const ma_node_t* node, uint8_t idx) {
    if (idx >= MT_SLOT_COUNT) {
        return NULL;
    }

    void* val = rcu_ptr_read((const rcu_ptr_t*)&node->slots[idx]);
    
    return mt_entry_to_slot(val);
}

___inline void ma_assign_slot(ma_node_t* node, uint8_t idx, void* val) {
    void* entry = val ? mt_slot_to_entry(val) : NULL;

    rcu_ptr_assign((rcu_ptr_t*)&node->slots[idx], entry);
}

typedef struct ma_gap_info {
    uint32_t gap_start;
    uint32_t gap_end;

    uint32_t gap_size;
} ma_gap_info_t;

void mt_init_cache(void);

void* mt_load(maple_tree_t* mt, uint32_t index);
int mt_store(maple_tree_t* mt, uint32_t index, uint32_t last, void* entry);

int mt_erase(maple_tree_t* mt, uint32_t index, uint32_t last);

void* mt_find(maple_tree_t* mt, uint32_t* index, uint32_t max);
void* mt_find_after(maple_tree_t* mt, uint32_t index);

int mt_next(maple_tree_t* mt, uint32_t* index);
int mt_prev(maple_tree_t* mt, uint32_t* index);

int mt_gap_find(maple_tree_t* mt, uint32_t size, uint32_t* out_index, uint32_t floor, uint32_t ceiling);

void mt_destroy(maple_tree_t* mt);

#ifdef __cplusplus
}
#endif

#endif
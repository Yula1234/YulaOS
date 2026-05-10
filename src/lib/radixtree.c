/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/radixtree.h>
#include <lib/compiler.h>
#include <lib/string.h>
#include <lib/dlist.h>

#include <kernel/panic.h>

#include <hal/align.h>

#include <mm/heap.h>

static __cacheline_aligned kmem_cache_t* g_radix_node_cache = NULL;
static __cacheline_aligned spinlock_t g_radix_cache_lock;

static void ensure_cache_initialized(void) {
    if (likely(g_radix_node_cache != NULL))
        return;

    {
    	guard(spinlock_safe)(&g_radix_cache_lock);

	    if (g_radix_node_cache == NULL)
	        g_radix_node_cache =
	    		kmem_cache_create("radix_node", sizeof(radix_node_t), 64u, 0u);

    }

    if (unlikely(g_radix_node_cache == NULL))
        panic("RADIX: Failed to allocate node cache");
}

static radix_node_t* radix_node_alloc(void) {
    ensure_cache_initialized();

    radix_node_t* node =
    		(radix_node_t*)kmem_cache_alloc(g_radix_node_cache);

    if (unlikely(!node))
        return NULL;

    memset(node, 0, sizeof(radix_node_t));

    for (uint32_t i = 0u; i < RADIX_TREE_MAP_SIZE; i++)
        rcu_ptr_init(&node->slots_[i]);

    return node;
}

static void radix_node_free_rcu_cb(rcu_head_t* head) {
    if (unlikely(!head))
        return;

    radix_node_t* node = container_of(head, radix_node_t, rcu_);

    kmem_cache_free(g_radix_node_cache, node);
}

___inline uint32_t max_key_for_shift(uint8_t shift) {
    const uint32_t total_bits = shift + RADIX_TREE_MAP_SHIFT;

    if (total_bits >= 32u)
        return 0xFFFFFFFFu;

    return (1u << total_bits) - 1u;
}

static int radix_tree_grow(radix_tree_t* tree, uint32_t key) {
    radix_node_t* root = (radix_node_t*)rcu_ptr_read(&tree->root_);

    uint8_t current_shift = 0u;

    if (root != NULL)
        current_shift = root->shift_;

    while (key > max_key_for_shift(current_shift)) {
        radix_node_t* new_root = radix_node_alloc();

        if (unlikely(!new_root))
            return -1;

        new_root->shift_ = current_shift + RADIX_TREE_MAP_SHIFT;
        new_root->offset_ = 0u;
        new_root->count_ = 0u;
        new_root->parent_ = NULL;

        if (root != NULL) {
            root->parent_ = new_root;
            root->offset_ = 0u;

            rcu_ptr_assign(&new_root->slots_[0], root);

            new_root->count_ = 1u;
        }

        rcu_ptr_assign(&tree->root_, new_root);
        
        root = new_root;
        current_shift = new_root->shift_;
        
        tree->max_depth_++;
    }

    /*
     * If the tree was completely empty, establish the first root node
     * at depth 0 (leaf level).
     */
    if (root == NULL) {
        radix_node_t* initial_root = radix_node_alloc();

        if (unlikely(!initial_root))
            return -1;

        initial_root->shift_ = 0u;
        initial_root->offset_ = 0u;
        initial_root->count_ = 0u;
        initial_root->parent_ = NULL;

        rcu_ptr_assign(&tree->root_, initial_root);
        tree->max_depth_ = 1u;
    }

    return 0;
}

static void radix_tree_shrink(radix_tree_t* tree) {
    for (;;) {
        radix_node_t* root = (radix_node_t*)rcu_ptr_read(&tree->root_);

        if (unlikely(!root))
            break;

        if (root->count_ != 1u
            || root->shift_ == 0u)
            break;

        radix_node_t* child = (radix_node_t*)rcu_ptr_read(&root->slots_[0]);

        if (unlikely(!child))
            break;

        child->parent_ = NULL;
        child->offset_ = 0u;

        rcu_ptr_assign(&tree->root_, child);

        tree->max_depth_--;

        call_rcu(&root->rcu_, radix_node_free_rcu_cb);
    }
}

void radix_tree_init(radix_tree_t* tree) {
    if (unlikely(!tree))
        return;

    spinlock_init(&tree->lock_);

    rcu_ptr_init(&tree->root_);
    
    tree->max_depth_ = 0u;

    if (g_radix_node_cache == NULL)
        spinlock_init(&g_radix_cache_lock);
}

void radix_tree_destroy(radix_tree_t* tree) {
    if (unlikely(!tree))
        return;

    radix_node_t* root = (radix_node_t*)rcu_ptr_read(&tree->root_);

    if (unlikely(root != NULL))
        panic("RADIX: Attempted to destroy a non-empty radix tree");

    tree->max_depth_ = 0u;
}

int radix_tree_insert(radix_tree_t* tree, uint32_t key, void* value) {
    if (unlikely(!tree)
        || unlikely(!value))
        return -1;

    {
    	guard(spinlock_safe)(&tree->lock_);

		if (radix_tree_grow(tree, key) != 0)
		    return -1;

		radix_node_t* current =
				(radix_node_t*)rcu_ptr_read(&tree->root_);

		while (current->shift_ > 0u) {
		    const uint8_t shift = current->shift_;
		    const uint8_t offset =
		    		(uint8_t)((key >> shift) & RADIX_TREE_MAP_MASK);

		    radix_node_t* child =
		    		(radix_node_t*)rcu_ptr_read(&current->slots_[offset]);

		    if (child == NULL) {
		        child = radix_node_alloc();

		        if (unlikely(!child))
		            return -1;

		        child->shift_ = shift - RADIX_TREE_MAP_SHIFT;
		        child->offset_ = offset;
		        child->parent_ = current;

		        rcu_ptr_assign(&current->slots_[offset], child);
		        current->count_++;
		    }

		    current = child;
		}

		const uint8_t leaf_offset = (uint8_t)(key & RADIX_TREE_MAP_MASK);

		void* existing = rcu_ptr_read(&current->slots_[leaf_offset]);

		if (unlikely(existing != NULL))
		    return -1; /* Collision */

		rcu_ptr_assign(&current->slots_[leaf_offset], value);
		current->count_++;

	}

    return 0;
}

void* radix_tree_lookup(radix_tree_t* tree, uint32_t key) {
	if (unlikely(!tree))
        return NULL;
	
	void* res = NULL;

    rcu_read_lock();

    radix_node_t* current = (radix_node_t*)rcu_ptr_read(&tree->root_);

    if (unlikely(current == NULL))
        goto unlock_rcu;

    if (key > max_key_for_shift(current->shift_))
        goto unlock_rcu;

    while (current->shift_ > 0u) {
        const uint8_t shift = current->shift_;
        const uint8_t offset = (uint8_t)((key >> shift) & RADIX_TREE_MAP_MASK);

        current = (radix_node_t*)rcu_ptr_read(&current->slots_[offset]);

        if (current == NULL)
            goto unlock_rcu;
    }

    const uint8_t leaf_offset = (uint8_t)(key & RADIX_TREE_MAP_MASK);

    res = rcu_ptr_read(&current->slots_[leaf_offset]);

unlock_rcu:
	rcu_read_unlock();

	return res;
}

void* radix_tree_find_next(radix_tree_t* tree, uint32_t* inout_key) {
    if (unlikely(!tree
        || !inout_key))
        return NULL;

    uint32_t key = *inout_key;

    rcu_read_lock();

restart:
    radix_node_t* current = (radix_node_t*)rcu_ptr_read(&tree->root_);

    if (unlikely(current == NULL))
        goto ret_null;

    if (unlikely(key > max_key_for_shift(current->shift_)))
        goto ret_null;

    while (current->shift_ > 0u) {
        const uint8_t shift = current->shift_;
        const uint8_t offset = (uint8_t)((key >> shift) & RADIX_TREE_MAP_MASK);

        radix_node_t* child = NULL;

        for (uint8_t i = offset; i < RADIX_TREE_MAP_SIZE; i++) {
            child = (radix_node_t*)rcu_ptr_read(&current->slots_[i]);

            if (child != NULL) {
                if (i > offset) {
                    const uint64_t mask = (1ull << (shift + RADIX_TREE_MAP_SHIFT)) - 1ull;
                    
                    key = (uint32_t)((key & ~mask) | ((uint64_t)i << shift));
                }

                break;
            }
        }

        if (child == NULL) {
            const uint8_t parent_shift = shift + RADIX_TREE_MAP_SHIFT;
            
            if (parent_shift >= 32u)
                goto ret_null;

            const uint64_t step = 1ull << parent_shift;
            const uint64_t next_key = (key & ~(step - 1ull)) + step;

            if (next_key > 0xFFFFFFFFull)
                goto ret_null;

            key = (uint32_t)next_key;
            
            goto restart;
        }

        current = child;
    }

    const uint8_t leaf_offset = (uint8_t)(key & RADIX_TREE_MAP_MASK);

    for (uint8_t i = leaf_offset; i < RADIX_TREE_MAP_SIZE; i++) {
        void* value = rcu_ptr_read(&current->slots_[i]);

        if (value != NULL) {
            key = (key & ~RADIX_TREE_MAP_MASK) | i;
            *inout_key = key;

            rcu_read_unlock();

            return value;
        }
    }

    const uint64_t leaf_step = RADIX_TREE_MAP_SIZE;
    const uint64_t next_leaf_key = (key & ~RADIX_TREE_MAP_MASK) + leaf_step;

    if (next_leaf_key > 0xFFFFFFFFull)
        goto ret_null;

    key = (uint32_t)next_leaf_key;
    
    goto restart;

ret_null:
    rcu_read_unlock();
    
    return NULL;
}

void* radix_tree_remove(radix_tree_t* tree, uint32_t key) {
    if (unlikely(!tree))
        return NULL;

    void* value = NULL;

    {
    	guard(spinlock_safe)(&tree->lock_);

	    radix_node_t* current = (radix_node_t*)rcu_ptr_read(&tree->root_);

	    if (unlikely(current == NULL)) 
	    	return NULL;

	    if (key > max_key_for_shift(current->shift_))
	        return NULL;

	    while (current->shift_ > 0u) {
	        const uint8_t shift = current->shift_;
	        const uint8_t offset =
	        		(uint8_t)((key >> shift) & RADIX_TREE_MAP_MASK);

	        current = (radix_node_t*)rcu_ptr_read(&current->slots_[offset]);

	        if (current == NULL)
	            return NULL;
	    }

	    const uint8_t leaf_offset = (uint8_t)(key & RADIX_TREE_MAP_MASK);

	    value = rcu_ptr_read(&current->slots_[leaf_offset]);

	    if (unlikely(value == NULL))
	        return NULL;

	    rcu_ptr_assign(&current->slots_[leaf_offset], NULL);
	    current->count_--;

	    while (current->count_ == 0u) {
	        radix_node_t* parent = current->parent_;

	        if (parent == NULL) {
	            rcu_ptr_assign(&tree->root_, NULL);
	            tree->max_depth_ = 0u;
	            
	            call_rcu(&current->rcu_, radix_node_free_rcu_cb);
	            break;
	        }

	        rcu_ptr_assign(&parent->slots_[current->offset_], NULL);
	        parent->count_--;

	        call_rcu(&current->rcu_, radix_node_free_rcu_cb);
	        
	        current = parent;
	    }

	    if (rcu_ptr_read(&tree->root_) != NULL)
	        radix_tree_shrink(tree);
    }

    return value;
}
/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef _LIB_LFSLIST_H
#define _LIB_LFSLIST_H

#include <lib/compiler.h>

/*
 * Simple Lock-Free Singly-Linked List.
 * 
 * No ABA protection. 
 * Safe only for append-only collections or structures where 
 * extracted nodes are not concurrently re-inserted.
 */

typedef struct lfsnode {
    struct lfsnode* next_;
} lfsnode_t;

typedef struct {
    lfsnode_t* volatile head_;
} lfslist_head_t;

/**
 * lfslist_init() - Initialize a simple lock-free list.
 */
___inline void lfslist_init(lfslist_head_t* list) {
    list->head_ = 0;
}

/**
 * lfslist_push() - Atomically push a node to the list.
 */
___inline void lfslist_push(lfslist_head_t* list, lfsnode_t* node) {
    lfsnode_t* old = __atomic_load_n(&list->head_, __ATOMIC_RELAXED);

    do {
        node->next_ = old;
    } while (!__atomic_compare_exchange_n(
                 &list->head_, &old, node, 1 /* weak */, 
                 __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/**
 * lfslist_pop_all() - Atomically extract the entire list.
 * 
 * Returns the old head and sets the list to empty. 
 */
___inline lfsnode_t* lfslist_pop_all(lfslist_head_t* list) {
    return __atomic_exchange_n(&list->head_, 0, __ATOMIC_ACQUIRE);
}

#endif
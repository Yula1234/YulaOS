/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef _LIB_LFLIST_H
#define _LIB_LFLIST_H

#include <lib/compiler.h>
#include <lib/tagged_ptr.h>

/*
 * Intrusive Lock-Free Singly Linked List (Treiber Stack).
 * ABA-safe for 32-bit x86 via Tagged Pointers.
 */

typedef struct lfnode {
    struct lfnode* next_;
} lfnode_t;

typedef struct {
    volatile tagged_ptr_t head_;
} __attribute__((aligned(8))) lflist_head_t;

#define LFLIST_INIT_VAL { { 0, 0 } }

___inline void lflist_init(lflist_head_t* list) {
    list->head_.ptr_     = 0;
    list->head_.version_ = 0u;
}

___inline void lflist_push(lflist_head_t* list, lfnode_t* node) {
    tagged_ptr_t old, desired;
    do {
        old = tagged_ptr_load(&list->head_);
        
        node->next_ = (lfnode_t*)old.ptr_;

        desired.ptr_     = node;
        desired.version_ = old.version_ + 1u;
    } while (!tagged_ptr_cas(&list->head_, old, desired));
}

___inline lfnode_t* lflist_pop(lflist_head_t* list) {
    tagged_ptr_t old, desired;
    do {
        old = tagged_ptr_load(&list->head_);
        
        if (unlikely(!old.ptr_)) {
            return 0;
        }

        desired.ptr_     = ((lfnode_t*)old.ptr_)->next_;
        desired.version_ = old.version_ + 1u;
    } while (!tagged_ptr_cas(&list->head_, old, desired));

    return (lfnode_t*)old.ptr_;
}

___inline void lflist_push_batch(lflist_head_t* list, lfnode_t* first, lfnode_t* last) {
    tagged_ptr_t old, desired;
    do {
        old = tagged_ptr_load(&list->head_);
        
        last->next_ = (lfnode_t*)old.ptr_;

        desired.ptr_     = first;
        desired.version_ = old.version_ + 1u;
    } while (!tagged_ptr_cas(&list->head_, old, desired));
}

___inline lfnode_t* lflist_pop_batch(lflist_head_t* list, uint32_t batch, uint32_t* out_count) {
    lfnode_t* first = 0;
    lfnode_t* last  = 0;

    uint32_t cnt = 0u;

    while (cnt < batch) {
        lfnode_t* node = lflist_pop(list);
        
        if (unlikely(!node)) {
            break;
        }

        if (unlikely(!first)) {
            first = node;
            last = node;
        } else {
            last->next_ = node;
            last = node;
        }

        cnt++;
    }

    if (last) {
        last->next_ = 0;
    }

    *out_count = cnt;

    return first;
}

#endif
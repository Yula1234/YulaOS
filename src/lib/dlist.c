// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "dlist.h"

static inline void __dlist_add(dlist_head_t *new_node,
                               dlist_head_t *prev,
                               dlist_head_t *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

void dlist_add(dlist_head_t *new_node, dlist_head_t *head) {
    __dlist_add(new_node, head, head->next);
}

void dlist_add_tail(dlist_head_t *new_node, dlist_head_t *head) {
    __dlist_add(new_node, head->prev, head);
}

static inline void __dlist_del(dlist_head_t *prev, dlist_head_t *next) {
    next->prev = prev;
    prev->next = next;
}

void dlist_del(dlist_head_t *entry) {
    __dlist_del(entry->prev, entry->next);
    entry->next = 0;
    entry->prev = 0;
}

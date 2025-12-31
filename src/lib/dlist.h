// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef LIB_DLIST_H
#define LIB_DLIST_H

#include <stddef.h>
#include <stdint.h>

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({ \
    const typeof( ((type *)0)->member ) *__mptr = (ptr); \
    (type *)( (char *)__mptr - offsetof(type,member) );})
#endif

typedef struct dlist_head {
    struct dlist_head *next, *prev;
} dlist_head_t;

static inline void dlist_init(dlist_head_t *list) {
    list->next = list;
    list->prev = list;
}

void dlist_add(dlist_head_t *new, dlist_head_t *head);
void dlist_add_tail(dlist_head_t *new, dlist_head_t *head);

void dlist_del(dlist_head_t *entry);

static inline int dlist_empty(const dlist_head_t *head) {
    return head->next == head;
}

#define dlist_for_each_entry(pos, head, member) \
    for (pos = container_of((head)->next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = container_of(pos->member.next, typeof(*pos), member))

#define dlist_for_each_entry_safe(pos, n, head, member) \
    for (pos = container_of((head)->next, typeof(*pos), member), \
         n = container_of(pos->member.next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = container_of(n->member.next, typeof(*n), member))

#define dlist_for_each_entry_reverse(pos, head, member) \
    for (pos = container_of((head)->prev, typeof(*pos), member); \
         &pos->member != (head); \
         pos = container_of(pos->member.prev, typeof(*pos), member))

#endif
// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef LIB_DLIST_H
#define LIB_DLIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <type_traits>
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifdef __cplusplus
#define DLIST_TYPEOF(pos) std::remove_reference<decltype(*(pos))>::type
#else
#define DLIST_TYPEOF(pos) typeof(*(pos))
#endif

#ifndef container_of
#ifdef __cplusplus
#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))
#else
#define container_of(ptr, type, member) ({ \
    const typeof( ((type *)0)->member ) *__mptr = (ptr); \
    (type *)( (char *)__mptr - offsetof(type,member) );})
#endif
#endif

typedef struct dlist_head {
    struct dlist_head *next, *prev;
} dlist_head_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline void dlist_init(dlist_head_t *list) {
    list->next = list;
    list->prev = list;
}

void dlist_add(dlist_head_t *new_node, dlist_head_t *head);
void dlist_add_tail(dlist_head_t *new_node, dlist_head_t *head);

void dlist_del(dlist_head_t *entry);

static inline int dlist_empty(const dlist_head_t *head) {
    return head->next == head;
}

#ifdef __cplusplus
}
#endif

#define dlist_for_each_entry(pos, head, member) \
    for (dlist_head_t* __it = (head)->next; \
         __it && __it != (head) && ((pos) = container_of(__it, DLIST_TYPEOF(pos), member), 1); \
         __it = __it->next)

#define dlist_for_each_entry_safe(pos, n, head, member) \
    for (dlist_head_t* __it = (head)->next, *__next = 0; \
         __it && __it != (head) && ((__next = __it->next), \
         (pos) = container_of(__it, DLIST_TYPEOF(pos), member), \
         (n) = (__next && __next != (head)) ? container_of(__next, DLIST_TYPEOF(n), member) : 0, 1); \
         __it = __next)

#define dlist_for_each_entry_reverse(pos, head, member) \
    for (pos = container_of((head)->prev, DLIST_TYPEOF(pos), member); \
         &pos->member != (head); \
         pos = container_of(pos->member.prev, DLIST_TYPEOF(pos), member))

#endif

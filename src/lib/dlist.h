// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef LIB_DLIST_H
#define LIB_DLIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <lib/cpp/type_traits.h>
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#ifdef __cplusplus
#define DLIST_TYPEOF(pos) kernel::remove_reference<decltype(*(pos))>::type
#else
#define DLIST_TYPEOF(pos) typeof(*(pos))
#endif

#ifndef container_of
#ifdef __cplusplus
#define container_of(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))
#else
#define container_of(ptr, type, member) ({ \
    const typeof(((type*)0)->member)* __mptr = (ptr); \
    (type*)((char*)__mptr - offsetof(type, member)); \
})
#endif
#endif

typedef struct dlist_head {
    struct dlist_head* next;
    struct dlist_head* prev;
} dlist_head_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline void dlist_init(dlist_head_t* list) {
    list->next = list;
    list->prev = list;
}

static inline int dlist_node_linked(const dlist_head_t* node) {
    return node && node->next && node->prev;
}

static inline int dlist_unlink_consistent(dlist_head_t* node) {
    if (!node || !node->prev || !node->next) {
        return 0;
    }

    dlist_head_t* prev = node->prev;
    dlist_head_t* next = node->next;

    if (!prev || !next) {
        return 0;
    }

    if (prev->next != node || next->prev != node) {
        return 0;
    }

    prev->next = next;
    next->prev = prev;

    node->next = 0;
    node->prev = 0;

    return 1;
}

static inline int dlist_unlink_consistent_checked(
    dlist_head_t* node,
    int (*node_valid)(const dlist_head_t*)
) {
    if (!node || !node->prev || !node->next) {
        return 0;
    }

    dlist_head_t* prev = node->prev;
    dlist_head_t* next = node->next;

    if (!prev || !next) {
        return 0;
    }

    if (node_valid) {
        if (!node_valid(prev) || !node_valid(next)) {
            return 0;
        }
    }

    if (prev->next != node || next->prev != node) {
        return 0;
    }

    prev->next = next;
    next->prev = prev;

    node->next = 0;
    node->prev = 0;

    return 1;
}

static inline int dlist_remove_node_if_present(dlist_head_t* head, dlist_head_t* node) {
    if (!head || !node) {
        return 0;
    }

    if (dlist_unlink_consistent(node)) {
        return 1;
    }

    dlist_head_t* it = head->next;

    while (it && it != head) {
        if (it == node) {
            dlist_head_t* prev = it->prev;
            dlist_head_t* next = it->next;

            if (prev && next) {
                next->prev = prev;
                prev->next = next;
            }

            it->next = 0;
            it->prev = 0;

            return 1;
        }

        it = it->next;
    }

    return 0;
}

static inline int dlist_remove_node_if_present_checked(
    dlist_head_t* head,
    dlist_head_t* node,
    int (*node_valid)(const dlist_head_t*),
    void (*on_corrupt)(const char*)
) {
    if (!head || !node) {
        return 0;
    }

    if (dlist_unlink_consistent_checked(node, node_valid)) {
        return 1;
    }

    dlist_head_t* it = head->next;

    while (it && it != head) {
        if (node_valid && !node_valid(it)) {
            if (on_corrupt) {
                on_corrupt("DLIST: corrupted list (invalid iter)");
            }

            return 0;
        }

        if (it == node) {
            dlist_head_t* prev = it->prev;
            dlist_head_t* next = it->next;

            if (prev && next) {
                if (node_valid) {
                    if (!node_valid(prev) || !node_valid(next)) {
                        if (on_corrupt) {
                            on_corrupt("DLIST: corrupted list (invalid links)");
                        }

                        return 0;
                    }
                }

                next->prev = prev;
                prev->next = next;
            }

            it->next = 0;
            it->prev = 0;

            return 1;
        }

        dlist_head_t* nxt = it->next;

        if (nxt && nxt != head && node_valid && !node_valid(nxt)) {
            if (on_corrupt) {
                on_corrupt("DLIST: corrupted list (invalid next)");
            }

            return 0;
        }

        it = nxt;
    }

    return 0;
}

void dlist_add(dlist_head_t* new_node, dlist_head_t* head);
void dlist_add_tail(dlist_head_t* new_node, dlist_head_t* head);

void dlist_del(dlist_head_t* entry);

static inline int dlist_empty(const dlist_head_t* head) {
    return head->next == head;
}

#ifdef __cplusplus
}
#endif

#define dlist_for_each_entry(pos, head, member) \
    for (dlist_head_t* __it = (head)->next; \
         __it && __it != (head) \
            && ((pos) = container_of(__it, DLIST_TYPEOF(pos), member), 1); \
         __it = __it->next)

#define dlist_for_each_entry_safe(pos, n, head, member) \
    for (dlist_head_t* __it = (head)->next, *__next = 0; \
         __it && __it != (head) \
            && ((__next = __it->next), \
                (pos) = container_of(__it, DLIST_TYPEOF(pos), member), \
                (n) = (__next && __next != (head)) \
                    ? container_of(__next, DLIST_TYPEOF(n), member) \
                    : 0, \
                1); \
         __it = __next)

#define dlist_for_each_entry_reverse(pos, head, member) \
    for (pos = container_of((head)->prev, DLIST_TYPEOF(pos), member); \
         &pos->member != (head); \
         pos = container_of(pos->member.prev, DLIST_TYPEOF(pos), member))

#endif

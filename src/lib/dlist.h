/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_DLIST_H
#define LIB_DLIST_H

#include <lib/compiler.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <lib/cpp/type_traits.h>
#define dlist_likely(x)   kernel::likely(x)
#define dlist_unlikely(x) kernel::unlikely(x)
#else
#define dlist_likely(x)   likely(x)
#define dlist_unlikely(x) unlikely(x)
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

#define dlist_prefetch(addr) __builtin_prefetch((addr), 0, 1)

typedef struct dlist_head {
    struct dlist_head* next;
    struct dlist_head* prev;
} dlist_head_t;

#ifdef __cplusplus
extern "C" {
#endif

___inline void dlist_init(dlist_head_t* list) {
    list->next = list;
    list->prev = list;
}

___inline int dlist_node_linked(const dlist_head_t* node) {
    return dlist_likely(node != 0) && node->next && node->prev;
}

___inline int dlist_empty(const dlist_head_t* head) {
    return head->next == head;
}

___inline void __dlist_add(dlist_head_t *new_node,
                           dlist_head_t *prev,
                           dlist_head_t *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

___inline void dlist_add(dlist_head_t *new_node, dlist_head_t *head) {
    __dlist_add(new_node, head, head->next);
}

___inline void dlist_add_tail(dlist_head_t *new_node, dlist_head_t *head) {
    __dlist_add(new_node, head->prev, head);
}

___inline void __dlist_del(dlist_head_t *prev, dlist_head_t *next) {
    next->prev = prev;
    prev->next = next;
}

___inline void dlist_del(dlist_head_t *entry) {
    __dlist_del(entry->prev, entry->next);
    entry->next = 0;
    entry->prev = 0;
}

___inline int dlist_unlink_consistent(dlist_head_t* node) {
    if (dlist_unlikely(!node)) {
        return 0;
    }
    
    dlist_head_t* prev = node->prev;
    dlist_head_t* next = node->next;

    if (dlist_unlikely(!prev
        || !next)) {
        return 0;
    }

    if (dlist_unlikely(prev->next != node
        || next->prev != node)) {
        return 0;
    }

    prev->next = next;
    next->prev = prev;
    node->next = 0;
    node->prev = 0;

    return 1;
}

___inline int dlist_unlink_consistent_checked(dlist_head_t* node,int (*node_valid)(const dlist_head_t*)) {
    if (dlist_unlikely(!node)) {
        return 0;
    }

    dlist_head_t* prev = node->prev;
    dlist_head_t* next = node->next;

    if (dlist_unlikely(!prev
        || !next)) {
        return 0;
    }

    if (node_valid) {
        if (dlist_unlikely(!node_valid(prev)
            || !node_valid(next))) {
            return 0;
        }
    }

    if (dlist_unlikely(prev->next != node
        || next->prev != node)) {
        return 0;
    }

    prev->next = next;
    next->prev = prev;
    node->next = 0;
    node->prev = 0;

    return 1;
}

___inline int dlist_remove_node_if_present(dlist_head_t* head, dlist_head_t* node) {
    if (dlist_unlikely(!head
        || !node)) {
        return 0;
    }

    if (dlist_likely(dlist_unlink_consistent(node))) {
        return 1;
    }

    dlist_head_t* it = head->next;

    while (dlist_likely(it != 0)
        && dlist_likely(it != head)) {

        dlist_head_t* next = it->next;
        dlist_prefetch(next);

        if (dlist_unlikely(it == node)) {
            dlist_head_t* p = it->prev;

            if (dlist_likely(p && next)) {
                next->prev = p;
                p->next = next;
            }
            
            it->next = 0;
            it->prev = 0;
            return 1;
        }

        it = next;
    }

    return 0;
}

___inline int dlist_remove_node_if_present_checked(dlist_head_t* head, dlist_head_t* node,
    int (*node_valid)(const dlist_head_t*), void (*on_corrupt)(const char*)
) {
    if (dlist_unlikely(!head
        || !node)) {
        return 0;
    }

    if (dlist_likely(dlist_unlink_consistent_checked(node, node_valid))) {
        return 1;
    }

    dlist_head_t* it = head->next;

    while (dlist_likely(it != 0)
        && dlist_likely(it != head)) {
        
        dlist_head_t* nxt = it->next;
        dlist_prefetch(nxt);

        if (node_valid
            && dlist_unlikely(!node_valid(it))) {
            
            if (on_corrupt) {
                on_corrupt("DLIST: corrupted list (invalid iter)");
            }
            
            return 0;
        }

        if (dlist_unlikely(it == node)) {
            dlist_head_t* p = it->prev;

            if (dlist_likely(p
                && nxt)) {
                if (node_valid
                    && dlist_unlikely(!node_valid(p)
                    || !node_valid(nxt))) {
                    
                    if (on_corrupt) {
                        on_corrupt("DLIST: corrupted list (invalid links)");
                    }
                    
                    return 0;
                }
            
                nxt->prev = p;
                p->next = nxt;
            }
            
            it->next = 0;
            it->prev = 0;
            return 1;
        }

        if (dlist_unlikely(nxt
            && nxt != head
            && node_valid
            && !node_valid(nxt))) {

            if (on_corrupt) {
                on_corrupt("DLIST: corrupted list (invalid next)");
            }
            
            return 0;
        }

        it = nxt;
    }

    return 0;
}

#ifdef __cplusplus
}
#endif

#define dlist_for_each_entry(pos, head, member) \
    for (dlist_head_t* __it = (head)->next; \
         dlist_likely(__it != 0) && dlist_likely(__it != (head)) \
            && (dlist_prefetch(__it->next), 1) \
            && ((pos) = container_of(__it, DLIST_TYPEOF(pos), member), 1); \
         __it = __it->next)

#define dlist_for_each_entry_safe(pos, n, head, member) \
    for (dlist_head_t* __it = (head)->next, *__next = 0; \
         dlist_likely(__it != 0) && dlist_likely(__it != (head)) \
            && ((__next = __it->next), 1) \
            && (dlist_prefetch(__next), 1) \
            && ((pos) = container_of(__it, DLIST_TYPEOF(pos), member), 1) \
            && ((n) = (__next && __next != (head)) \
                    ? container_of(__next, DLIST_TYPEOF(n), member) \
                    : 0, 1); \
         __it = __next)

#define dlist_for_each_entry_reverse(pos, head, member) \
    for (dlist_head_t* __it = (head)->prev; \
         dlist_likely(__it != 0) && dlist_likely(__it != (head)) \
            && (dlist_prefetch(__it->prev), 1) \
            && ((pos) = container_of(__it, DLIST_TYPEOF(pos), member), 1); \
         __it = __it->prev)

#endif
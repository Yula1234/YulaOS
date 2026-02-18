// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef _LIB_RBTREE_H
#define _LIB_RBTREE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rb_node {
    uintptr_t  __parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_ROOT (struct rb_root) { 0 }
#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);

struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **rb_link)
{
    node->__parent_color = (uintptr_t)parent;
    node->rb_left = node->rb_right = 0;

    *rb_link = node;
}

#ifdef __cplusplus
}
#endif

#endif

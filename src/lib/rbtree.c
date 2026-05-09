/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */
/* Copyright (C) 2026 Yula1234 Rewritten to pure C */

#include <lib/compiler.h>
#include <lib/rbtree.h>

#include <stddef.h>
#include <stdint.h>

#define RB_RED   0UL
#define RB_BLACK 1UL

___inline struct rb_node* rb_parent_node(const struct rb_node* node) {
    return (struct rb_node*)(node->__parent_color & ~3UL);
}

___inline uintptr_t rb_color(const struct rb_node* node) {
    return node->__parent_color & 1UL;
}

___inline int rb_is_red(const struct rb_node* node) {
    return rb_color(node) == RB_RED;
}

___inline int rb_is_black(const struct rb_node* node) {
    return rb_color(node) == RB_BLACK;
}

___inline void rb_set_parent(struct rb_node* node, struct rb_node* parent) {
    node->__parent_color = (node->__parent_color & 3UL) | (uintptr_t)parent;
}

___inline void rb_set_color(struct rb_node* node, uintptr_t color) {
    node->__parent_color = (node->__parent_color & ~1UL) | color;
}

___inline void rb_set_black(struct rb_node* node) {
    node->__parent_color |= RB_BLACK;
}

___inline void rb_set_red(struct rb_node* node) {
    node->__parent_color &= ~RB_BLACK;
}

___inline void rb_rotate_left(
    struct rb_node* node, struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    struct rb_node* right = node->rb_right;
    struct rb_node* parent = rb_parent_node(node);

    if ((node->rb_right = right->rb_left) != NULL)
        rb_set_parent(right->rb_left, node);

    right->rb_left = node;

    rb_set_parent(right, parent);

    if (parent) {
        if (node == parent->rb_left)
            parent->rb_left = right;
        else
            parent->rb_right = right;
    } else {
        root->rb_node = right;
    }

    rb_set_parent(node, right);

    if (callbacks && callbacks->rotate)
        callbacks->rotate(node, right);
}

___inline void rb_rotate_right(
    struct rb_node* node, struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    struct rb_node* left = node->rb_left;
    struct rb_node* parent = rb_parent_node(node);

    if ((node->rb_left = left->rb_right) != NULL)
        rb_set_parent(left->rb_right, node);

    left->rb_right = node;

    rb_set_parent(left, parent);

    if (parent) {
        if (node == parent->rb_right)
            parent->rb_right = left;
        else
            parent->rb_left = left;
    } else {
        root->rb_node = left;
    }

    rb_set_parent(node, left);

    if (callbacks && callbacks->rotate)
        callbacks->rotate(node, left);
}

___inline void __rb_insert_color(
    struct rb_node* node, struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    struct rb_node* parent;
    struct rb_node* gparent;

    while ((parent = rb_parent_node(node)) != NULL 
           && rb_is_red(parent)) {
        
        gparent = rb_parent_node(parent);

        if (parent == gparent->rb_left) {
            struct rb_node* uncle = gparent->rb_right;

            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);

                rb_set_red(gparent);

                node = gparent;
                continue;
            }

            if (parent->rb_right == node) {
                rb_rotate_left(parent, root, callbacks);

                struct rb_node* tmp = parent;
                parent = node;
                node = tmp;
            }

            rb_set_black(parent);
            rb_set_red(gparent);

            rb_rotate_right(gparent, root, callbacks);
        } else {
            struct rb_node* uncle = gparent->rb_left;

            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);

                rb_set_red(gparent);

                node = gparent;
                continue;
            }

            if (parent->rb_left == node) {
                rb_rotate_right(parent, root, callbacks);

                struct rb_node* tmp = parent;

                parent = node;
                node = tmp;
            }

            rb_set_black(parent);
            rb_set_red(gparent);

            rb_rotate_left(gparent, root, callbacks);
        }
    }

    rb_set_black(root->rb_node);

    if (callbacks && callbacks->propagate)
        callbacks->propagate(node, NULL);
}

___inline void __rb_erase_color(
    struct rb_node* node,
    struct rb_node* parent,
    struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    struct rb_node* other;

    while ((!node || rb_is_black(node)) && node != root->rb_node) {
        if (parent->rb_left == node) {
            other = parent->rb_right;

            if (rb_is_red(other)) {
                rb_set_black(other);
                rb_set_red(parent);

                rb_rotate_left(parent, root, callbacks);

                other = parent->rb_right;
            }

            if ((!other->rb_left || rb_is_black(other->rb_left))
                && (!other->rb_right || rb_is_black(other->rb_right))) {
                
                rb_set_red(other);

                node = parent;
                parent = rb_parent_node(node);
            } else {
                if (!other->rb_right || rb_is_black(other->rb_right)) {
                    rb_set_black(other->rb_left);
                    rb_set_red(other);

                    rb_rotate_right(other, root, callbacks);

                    other = parent->rb_right;
                }

                rb_set_color(other, rb_color(parent));
                rb_set_black(parent);
                rb_set_black(other->rb_right);

                rb_rotate_left(parent, root, callbacks);

                node = root->rb_node;
                break;
            }
        } else {
            other = parent->rb_left;

            if (rb_is_red(other)) {
                rb_set_black(other);
                rb_set_red(parent);

                rb_rotate_right(parent, root, callbacks);

                other = parent->rb_left;
            }

            if ((!other->rb_left || rb_is_black(other->rb_left))
                && (!other->rb_right || rb_is_black(other->rb_right))) {
                
                rb_set_red(other);

                node = parent;
                parent = rb_parent_node(node);
            } else {
                if (!other->rb_left || rb_is_black(other->rb_left)) {
                    rb_set_black(other->rb_right);
                    rb_set_red(other);

                    rb_rotate_left(other, root, callbacks);

                    other = parent->rb_left;
                }

                rb_set_color(other, rb_color(parent));
                rb_set_black(parent);
                rb_set_black(other->rb_left);

                rb_rotate_right(parent, root, callbacks);

                node = root->rb_node;
                break;
            }
        }
    }

    if (node)
        rb_set_black(node);
}

___inline void __rb_erase(
    struct rb_node* node,
    struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    struct rb_node* child;
    struct rb_node* parent;
    struct rb_node* rebalance = NULL;
    
    uintptr_t color;

    if (!node->rb_left) {
        child = node->rb_right;
    } else if (!node->rb_right) {
        child = node->rb_left;
    } else {
        struct rb_node* old = node;
        struct rb_node* left;

        node = node->rb_right;

        while ((left = node->rb_left) != NULL) {
            node = left;
        }

        if (rb_parent_node(old)) {
            if (rb_parent_node(old)->rb_left == old)
                rb_parent_node(old)->rb_left = node;
            else
                rb_parent_node(old)->rb_right = node;
        } else {
            root->rb_node = node;
        }

        child = node->rb_right;
        parent = rb_parent_node(node);
        color = rb_color(node);

        if (parent == old) {
            parent = node;
        } else {
            if (child)
                rb_set_parent(child, parent);

            parent->rb_left = child;

            node->rb_right = old->rb_right;
            rb_set_parent(old->rb_right, node);
        }

        node->__parent_color = old->__parent_color;
        node->rb_left = old->rb_left;

        rb_set_parent(old->rb_left, node);

        if (callbacks && callbacks->copy)
            callbacks->copy(old, node);

        rebalance = parent;
        goto color_check;
    }

    parent = rb_parent_node(node);
    color = rb_color(node);

    if (child)
        rb_set_parent(child, parent);

    if (parent) {
        if (parent->rb_left == node)
            parent->rb_left = child;
        else
            parent->rb_right = child;
    } else {
        root->rb_node = child;
    }

    rebalance = parent;

color_check:
    if (color == RB_BLACK)
        __rb_erase_color(child, parent, root, callbacks);

    if (callbacks && callbacks->propagate)
        callbacks->propagate(rebalance, NULL);
}

void rb_insert_color(struct rb_node* node, struct rb_root* root) {
    __rb_insert_color(node, root, NULL);
}

void rb_insert_color_augmented(
    struct rb_node* node, struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    __rb_insert_color(node, root, callbacks);
}

void rb_erase(struct rb_node* node, struct rb_root* root) {
    __rb_erase(node, root, NULL);
}

void rb_erase_augmented(
    struct rb_node* node, struct rb_root* root,
    const struct rb_augment_callbacks* callbacks
) {
    __rb_erase(node, root, callbacks);
}

struct rb_node* rb_first(const struct rb_root* root) {
    if (!root)
        return NULL;

    struct rb_node* node = root->rb_node;

    if (!node)
        return NULL;

    while (node->rb_left)
        node = node->rb_left;

    return node;
}

struct rb_node* rb_last(const struct rb_root* root) {
    if (!root)
        return NULL;

    struct rb_node* node = root->rb_node;

    if (!node)
        return NULL;

    while (node->rb_right)
        node = node->rb_right;

    return node;
}

struct rb_node* rb_next(const struct rb_node* node) {
    if (!node)
        return NULL;

    if (rb_parent_node(node) == node)
        return NULL;

    if (node->rb_right) {
        node = node->rb_right;

        while (node->rb_left)
            node = node->rb_left;

        return (struct rb_node*)node;
    }

    struct rb_node* parent;

    while ((parent = rb_parent_node(node)) != NULL 
           && node == parent->rb_right)
        node = parent;

    return parent;
}

struct rb_node* rb_prev(const struct rb_node* node) {
    if (!node)
        return NULL;

    if (rb_parent_node(node) == node)
        return NULL;

    if (node->rb_left) {
        node = node->rb_left;

        while (node->rb_right)
            node = node->rb_right;

        return (struct rb_node*)node;
    }

    struct rb_node* parent;

    while ((parent = rb_parent_node(node)) != NULL 
           && node == parent->rb_left)
        node = parent;

    return parent;
}
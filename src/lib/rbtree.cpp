// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "rbtree.h"

namespace kernel {

class RbTree {
public:
    static void InsertColor(struct rb_node* node, struct rb_root* root);
    static void Erase(struct rb_node* node, struct rb_root* root);
    static struct rb_node* Next(const struct rb_node* node);
    static struct rb_node* Prev(const struct rb_node* node);
    static struct rb_node* First(const struct rb_root* root);
    static struct rb_node* Last(const struct rb_root* root);

private:
    static const uintptr_t RED = 0;
    static const uintptr_t BLACK = 1;

    static struct rb_node* Parent(const struct rb_node* node) {
        return (struct rb_node*)(node->__parent_color & ~3);
    }

    static uintptr_t Color(const struct rb_node* node) {
        return node->__parent_color & 1;
    }

    static bool IsRed(const struct rb_node* node) {
        return Color(node) == RED;
    }

    static bool IsBlack(const struct rb_node* node) {
        return Color(node) == BLACK;
    }

    static void SetParent(struct rb_node* node, struct rb_node* parent) {
        node->__parent_color = (node->__parent_color & 3) | (uintptr_t)parent;
    }

    static void SetColor(struct rb_node* node, uintptr_t color) {
        node->__parent_color = (node->__parent_color & ~1) | color;
    }

    static void SetBlack(struct rb_node* node) {
        node->__parent_color |= BLACK;
    }

    static void SetRed(struct rb_node* node) {
        node->__parent_color &= ~1;
    }

    static void RotateLeft(struct rb_node* node, struct rb_root* root);
    static void RotateRight(struct rb_node* node, struct rb_root* root);
    static void EraseColor(struct rb_node* node, struct rb_node* parent, struct rb_root* root);
};

void RbTree::RotateLeft(struct rb_node* node, struct rb_root* root) {
    struct rb_node* right = node->rb_right;
    struct rb_node* parent = Parent(node);

    if ((node->rb_right = right->rb_left))
        SetParent(right->rb_left, node);
    right->rb_left = node;

    SetParent(right, parent);

    if (parent) {
        if (node == parent->rb_left)
            parent->rb_left = right;
        else
            parent->rb_right = right;
    } else {
        root->rb_node = right;
    }
    SetParent(node, right);
}

void RbTree::RotateRight(struct rb_node* node, struct rb_root* root) {
    struct rb_node* left = node->rb_left;
    struct rb_node* parent = Parent(node);

    if ((node->rb_left = left->rb_right))
        SetParent(left->rb_right, node);
    left->rb_right = node;

    SetParent(left, parent);

    if (parent) {
        if (node == parent->rb_right)
            parent->rb_right = left;
        else
            parent->rb_left = left;
    } else {
        root->rb_node = left;
    }
    SetParent(node, left);
}

void RbTree::InsertColor(struct rb_node* node, struct rb_root* root) {
    struct rb_node *parent, *gparent;

    while ((parent = Parent(node)) && IsRed(parent)) {
        gparent = Parent(parent);

        if (parent == gparent->rb_left) {
            {
                struct rb_node* uncle = gparent->rb_right;
                if (uncle && IsRed(uncle)) {
                    SetBlack(uncle);
                    SetBlack(parent);
                    SetRed(gparent);
                    node = gparent;
                    continue;
                }
            }

            if (parent->rb_right == node) {
                struct rb_node* tmp;
                RotateLeft(parent, root);
                tmp = parent;
                parent = node;
                node = tmp;
            }

            SetBlack(parent);
            SetRed(gparent);
            RotateRight(gparent, root);
        } else {
            {
                struct rb_node* uncle = gparent->rb_left;
                if (uncle && IsRed(uncle)) {
                    SetBlack(uncle);
                    SetBlack(parent);
                    SetRed(gparent);
                    node = gparent;
                    continue;
                }
            }

            if (parent->rb_left == node) {
                struct rb_node* tmp;
                RotateRight(parent, root);
                tmp = parent;
                parent = node;
                node = tmp;
            }

            SetBlack(parent);
            SetRed(gparent);
            RotateLeft(gparent, root);
        }
    }

    SetBlack(root->rb_node);
}

void RbTree::EraseColor(struct rb_node* node, struct rb_node* parent, struct rb_root* root) {
    struct rb_node* other;

    while ((!node || IsBlack(node)) && node != root->rb_node) {
        if (parent->rb_left == node) {
            other = parent->rb_right;
            if (IsRed(other)) {
                SetBlack(other);
                SetRed(parent);
                RotateLeft(parent, root);
                other = parent->rb_right;
            }
            if ((!other->rb_left || IsBlack(other->rb_left)) &&
                (!other->rb_right || IsBlack(other->rb_right))) {
                SetRed(other);
                node = parent;
                parent = Parent(node);
            } else {
                if (!other->rb_right || IsBlack(other->rb_right)) {
                    SetBlack(other->rb_left);
                    SetRed(other);
                    RotateRight(other, root);
                    other = parent->rb_right;
                }
                SetColor(other, Color(parent));
                SetBlack(parent);
                SetBlack(other->rb_right);
                RotateLeft(parent, root);
                node = root->rb_node;
                break;
            }
        } else {
            other = parent->rb_left;
            if (IsRed(other)) {
                SetBlack(other);
                SetRed(parent);
                RotateRight(parent, root);
                other = parent->rb_left;
            }
            if ((!other->rb_left || IsBlack(other->rb_left)) &&
                (!other->rb_right || IsBlack(other->rb_right))) {
                SetRed(other);
                node = parent;
                parent = Parent(node);
            } else {
                if (!other->rb_left || IsBlack(other->rb_left)) {
                    SetBlack(other->rb_right);
                    SetRed(other);
                    RotateLeft(other, root);
                    other = parent->rb_left;
                }
                SetColor(other, Color(parent));
                SetBlack(parent);
                SetBlack(other->rb_left);
                RotateRight(parent, root);
                node = root->rb_node;
                break;
            }
        }
    }
    if (node)
        SetBlack(node);
}

void RbTree::Erase(struct rb_node* node, struct rb_root* root) {
    struct rb_node *child, *parent;
    uintptr_t color;

    if (!node->rb_left)
        child = node->rb_right;
    else if (!node->rb_right)
        child = node->rb_left;
    else {
        struct rb_node *old = node, *left;

        node = node->rb_right;
        while ((left = node->rb_left) != 0)
            node = left;

        if (Parent(old)) {
            if (Parent(old)->rb_left == old)
                Parent(old)->rb_left = node;
            else
                Parent(old)->rb_right = node;
        } else
            root->rb_node = node;

        child = node->rb_right;
        parent = Parent(node);
        color = Color(node);

        if (parent == old) {
            parent = node;
        } else {
            if (child)
                SetParent(child, parent);
            parent->rb_left = child;

            node->rb_right = old->rb_right;
            SetParent(old->rb_right, node);
        }

        node->__parent_color = old->__parent_color;
        node->rb_left = old->rb_left;
        SetParent(old->rb_left, node);

        goto color;
    }

    parent = Parent(node);
    color = Color(node);

    if (child)
        SetParent(child, parent);
    if (parent) {
        if (parent->rb_left == node)
            parent->rb_left = child;
        else
            parent->rb_right = child;
    } else
        root->rb_node = child;

color:
    if (color == BLACK)
        EraseColor(child, parent, root);
}

struct rb_node* RbTree::First(const struct rb_root* root) {
    struct rb_node* n = root->rb_node;
    if (!n) return 0;
    while (n->rb_left)
        n = n->rb_left;
    return n;
}

struct rb_node* RbTree::Last(const struct rb_root* root) {
    struct rb_node* n = root->rb_node;
    if (!n) return 0;
    while (n->rb_right)
        n = n->rb_right;
    return n;
}

struct rb_node* RbTree::Next(const struct rb_node* node) {
    struct rb_node* parent;

    if (Parent(node) == node)
        return 0;

    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left)
            node = node->rb_left;
        return (struct rb_node*)node;
    }

    while ((parent = Parent(node)) && node == parent->rb_right)
        node = parent;

    return parent;
}

struct rb_node* RbTree::Prev(const struct rb_node* node) {
    struct rb_node* parent;

    if (Parent(node) == node)
        return 0;

    if (node->rb_left) {
        node = node->rb_left;
        while (node->rb_right)
            node = node->rb_right;
        return (struct rb_node*)node;
    }

    while ((parent = Parent(node)) && node == parent->rb_left)
        node = parent;

    return parent;
}

}

extern "C" {

void rb_insert_color(struct rb_node *node, struct rb_root *root) {
    kernel::RbTree::InsertColor(node, root);
}

void rb_erase(struct rb_node *node, struct rb_root *root) {
    kernel::RbTree::Erase(node, root);
}

struct rb_node *rb_next(const struct rb_node *node) {
    return kernel::RbTree::Next(node);
}

struct rb_node *rb_prev(const struct rb_node *node) {
    return kernel::RbTree::Prev(node);
}

struct rb_node *rb_first(const struct rb_root *root) {
    return kernel::RbTree::First(root);
}

struct rb_node *rb_last(const struct rb_root *root) {
    return kernel::RbTree::Last(root);
}

}

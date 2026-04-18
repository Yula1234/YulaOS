/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/maple_tree.h>
#include <lib/compiler.h>
#include <lib/cpp/new.h>
#include <lib/dlist.h>

#include <kernel/locking/spinlock.h>

#include <mm/heap.h>

namespace {

constexpr uint32_t kMaxKey = 0xFFFFFFFFu;

static kmem_cache_t* g_ma_node_cache = nullptr;

ma_node_t* alloc_ma_node(uint32_t type) noexcept {
    if (!g_ma_node_cache) {
        return nullptr;
    }

    void* obj = kmem_cache_alloc(g_ma_node_cache);

    if (!obj) {
        return nullptr;
    }

    ma_node_t* node = new (obj) ma_node_t{};
    ma_set_type(node, type);

    return node;
}

___inline void free_ma_node(ma_node_t* node) noexcept {
    if (node) {
        node->~ma_node_t();

        kmem_cache_free(g_ma_node_cache, node);
    }
}

void free_ma_node_rcu(rcu_head_t* head) noexcept {
    if (!head) {
        return;
    }

    ma_node_t* node = container_of(head, ma_node_t, slots[0]);

    free_ma_node(node);
}

uint8_t slot_count(const ma_node_t* node) noexcept {
    if (!node) {
        return 0u;
    }

    uint8_t cnt = 0u;

    for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
        if (node->slots[i] != nullptr) {
            cnt++;
        }
    }

    return cnt;
}

void shift_slots_right(ma_node_t* node, uint8_t from, uint8_t count) noexcept {
    for (uint8_t i = MT_SLOT_COUNT; i > from + count; i--) {
        node->slots[i - 1u] = node->slots[i - 1u - count];
    }

    for (uint8_t i = 0u; i < count; i++) {
        node->slots[from + i] = nullptr;
    }
}

void shift_pivots_right(ma_node_t* node, uint8_t from, uint8_t count) noexcept {
    if (from >= MT_PIVOT_COUNT) {
        return;
    }

    for (uint8_t i = MT_PIVOT_COUNT - 1u; i >= from + count; i--) {
        node->pivots[i] = node->pivots[i - count];
    }

    uint8_t clear_end = from + count;

    if (clear_end > MT_PIVOT_COUNT) {
        clear_end = MT_PIVOT_COUNT;
    }

    for (uint8_t i = from; i < clear_end; i++) {
        node->pivots[i] = 0u;
    }
}

void shift_slots_left(ma_node_t* node, uint8_t from, uint8_t count) noexcept {
    for (uint8_t i = from; i + count < MT_SLOT_COUNT; i++) {
        node->slots[i] = node->slots[i + count];
    }

    for (uint8_t i = MT_SLOT_COUNT - count; i < MT_SLOT_COUNT; i++) {
        node->slots[i] = nullptr;
    }
}

void shift_pivots_left(ma_node_t* node, uint8_t from, uint8_t count) noexcept {
    if (from >= MT_PIVOT_COUNT) {
        return;
    }

    for (uint8_t i = from; i + count < MT_PIVOT_COUNT; i++) {
        node->pivots[i] = node->pivots[i + count];
    }

    uint8_t start_clear = MT_PIVOT_COUNT - count;

    if (start_clear < from) {
        start_clear = from;
    }

    for (uint8_t i = start_clear; i < MT_PIVOT_COUNT; i++) {
        node->pivots[i] = 0u;
    }
}

bool ma_descend(ma_state_t* mas, uint32_t index) noexcept {
    ma_node_t* node = mas->node;

    mas->depth = 0u;

    while (node && !ma_node_is_leaf(node)) {
        uint8_t idx = 0u;

        for (uint8_t i = 0u; i < MT_PIVOT_COUNT; i++) {
            if (index <= node->pivots[i]) {
                idx = i;
                break;
            }

            idx = i + 1u;

            if (i + 1u >= MT_PIVOT_COUNT
                || node->pivots[i + 1u] == 0u) {
                break;
            }
        }

        mas->offset = idx;

        void* child = ma_slot_rcu(node, idx);

        if (!child) {
            return false;
        }

        node = static_cast<ma_node_t*>(child);
        mas->node = node;

        mas->depth++;
    }

    if (node && ma_node_is_leaf(node)) {
        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            uint32_t pivot = (i < MT_PIVOT_COUNT) ? node->pivots[i] : kMaxKey;

            if (index <= pivot
                || node->slots[i] == nullptr) {
                mas->offset = i;

                return true;
            }
        }

        mas->offset = MT_SLOT_COUNT - 1u;

        return true;
    }

    return false;
}

void ma_recalc_pivots(ma_node_t* node) noexcept {
    if (!node) {
        return;
    }

    uint8_t last = MT_SLOT_COUNT;

    for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
        if (node->slots[i] == nullptr) {
            last = i;
            break;
        }
    }

    for (uint8_t i = 0u; i < MT_PIVOT_COUNT; i++) {
        if (i + 1u < last) {
            continue;
        }

        if (i + 1u == last) {
            node->pivots[i] = kMaxKey;
        } else {
            node->pivots[i] = 0u;
        }
    }
}

void ma_propagate_pivots(maple_tree_t* mt, ma_node_t* node) noexcept {
    while (node) {
        ma_recalc_pivots(node);

        ma_node_t* parent = ma_parent(node);

        if (parent) {
            uint8_t pslot = ma_parent_slot(node);

            if (pslot < MT_PIVOT_COUNT) {
                uint32_t max_val = 0;

                bool has_val = false;

                if (ma_node_is_leaf(node)) {
                    for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
                        if (node->slots[i]) {
                            uint32_t e_end = *((uint32_t*)mt_entry_to_slot(node->slots[i]) + 1);

                            if (!has_val
                                || e_end - 1u > max_val) {
                                max_val = e_end - 1u;

                                has_val = true;
                            }
                        }
                    }
                } else {
                    for (uint8_t i = 0; i < MT_PIVOT_COUNT; i++) {
                        if (node->pivots[i] != 0u) {
                            if (node->pivots[i] == kMaxKey) {
                                max_val = kMaxKey;

                                has_val = true;
                                break;
                            }

                            if (!has_val
                                || node->pivots[i] > max_val) {
                                max_val = node->pivots[i];

                                has_val = true;
                            }
                        }
                    }
                }

                if (has_val && parent->pivots[pslot] != kMaxKey) {
                    parent->pivots[pslot] = max_val;
                }
            }
        }

        if (mt->ma_root_node == node) {
            break;
        }

        node = parent;
    }
}

int split_node(
    maple_tree_t* mt, ma_node_t* orig,
    ma_node_t* parent, uint8_t slot_in_parent
) noexcept {
    uint8_t mid = MT_SLOT_COUNT / 2u;

    uint32_t orig_type = ma_node_is_leaf(orig) ? MT_NODE_LEAF : MT_NODE_RANGE;

    ma_node_t* right = alloc_ma_node(orig_type);

    if (!right) {
        return -1;
    }

    for (uint8_t i = mid; i < MT_SLOT_COUNT; i++) {
        right->slots[i - mid] = orig->slots[i];

        orig->slots[i] = nullptr;
    }

    for (uint8_t i = mid; i < MT_PIVOT_COUNT; i++) {
        right->pivots[i - mid] = orig->pivots[i];

        orig->pivots[i] = 0u;
    }

    uint32_t split_pivot = orig->pivots[mid - 1u];

    orig->pivots[mid - 1u] = kMaxKey;

    if (parent == nullptr) {
        ma_node_t* new_root = alloc_ma_node(MT_NODE_RANGE);

        if (!new_root) {
            for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
                if (right->slots[i] && !ma_node_is_leaf(right)) {
                    ma_set_parent(static_cast<ma_node_t*>(right->slots[i]), right, i);
                }
            }

            free_ma_node(right);

            return -1;
        }

        rcu_ptr_assign((rcu_ptr_t*)&new_root->slots[0], orig);
        rcu_ptr_assign((rcu_ptr_t*)&new_root->slots[1], right);

        new_root->pivots[0] = split_pivot;

        ma_set_parent(orig, new_root, 0u);
        ma_set_parent(right, new_root, 1u);

        rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, new_root);

        if (!ma_node_is_leaf(right)) {
            for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
                if (right->slots[i]) {
                    ma_set_parent(static_cast<ma_node_t*>(right->slots[i]), right, i);
                }
            }
        }

        ma_recalc_pivots(orig);
        ma_recalc_pivots(right);

        ma_propagate_pivots(mt, new_root);

        return 0;
    }

    uint8_t pslot = slot_in_parent;

    if (slot_count(parent) >= MT_SLOT_COUNT) {
        int rc = split_node(mt, parent, ma_parent(parent), ma_parent_slot(parent));

        if (rc < 0) {
            for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
                if (right->slots[i]) {
                    if (!ma_node_is_leaf(right)) {
                        ma_set_parent(static_cast<ma_node_t*>(right->slots[i]), orig, i + mid);
                    }

                    orig->slots[i + mid] = right->slots[i];
                }
            }

            for (uint8_t i = 0u; i < MT_PIVOT_COUNT; i++) {
                if (i + mid < MT_PIVOT_COUNT) {
                    orig->pivots[i + mid] = right->pivots[i];
                }
            }

            if (mid - 1u < MT_PIVOT_COUNT) {
                orig->pivots[mid - 1u] = split_pivot;
            }

            free_ma_node(right);

            return -1;
        }

        parent = ma_parent(orig);

        pslot = ma_parent_slot(orig);
    }

    shift_slots_right(parent, pslot + 1u, 1u);
    shift_pivots_right(parent, pslot, 1u);

    rcu_ptr_assign((rcu_ptr_t*)&parent->slots[pslot + 1u], right);

    parent->pivots[pslot] = split_pivot;

    ma_set_parent(right, parent, pslot + 1u);

    for (uint8_t i = pslot + 2u; i < MT_SLOT_COUNT; i++) {
        void* child = ma_slot_rcu(parent, i);

        if (child && !mt_is_entry_ptr(child)) {
            ma_set_parent(static_cast<ma_node_t*>(child), parent, i);
        }
    }

    if (!ma_node_is_leaf(right)) {
        for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
            if (right->slots[i]) {
                ma_set_parent(static_cast<ma_node_t*>(right->slots[i]), right, i);
            }
        }
    }

    ma_recalc_pivots(orig);
    ma_recalc_pivots(right);

    ma_propagate_pivots(mt, parent);

    return 0;
}

void rebalance_after_erase(maple_tree_t* mt, ma_node_t* parent) noexcept {
    while (parent) {
        uint8_t cnt = slot_count(parent);

        if (cnt == 0u) {
            ma_node_t* grandparent = ma_parent(parent);

            if (!grandparent) {
                rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, (void*)MA_ROOT_FLAG);

                call_rcu((rcu_head_t*)&parent->slots[0], free_ma_node_rcu);

                return;
            }

            uint8_t gp_slot = ma_parent_slot(parent);

            rcu_ptr_assign((rcu_ptr_t*)&grandparent->slots[gp_slot], nullptr);

            shift_slots_left(grandparent, gp_slot, 1u);
            shift_pivots_left(grandparent, gp_slot, 1u);

            for (uint8_t i = gp_slot; i < MT_SLOT_COUNT; i++) {
                if (grandparent->slots[i]) {
                    ma_node_t* sibling = static_cast<ma_node_t*>(ma_slot_rcu(grandparent, i));

                    if (!mt_is_entry_ptr(sibling)) {
                        ma_set_parent(sibling, grandparent, i);
                    }
                }
            }

            call_rcu((rcu_head_t*)&parent->slots[0], free_ma_node_rcu);

            parent = grandparent;

            continue;
        }

        if (cnt == 1u && parent == mt->ma_root_node) {
            void* child = nullptr;

            for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
                if (parent->slots[i]) {
                    child = ma_slot_rcu(parent, i);
                    break;
                }
            }

            if (child && !mt_is_entry_ptr(child)) {
                ma_node_t* child_node = static_cast<ma_node_t*>(child);

                ma_set_parent(child_node, nullptr, 0u);

                rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, child_node);

                call_rcu((rcu_head_t*)&parent->slots[0], free_ma_node_rcu);
            }

            return;
        }

        ma_recalc_pivots(parent);

        parent = ma_parent(parent);
    }
}

void destroy_subtree(ma_node_t* node) noexcept {
    if (!node) {
        return;
    }

    if (!ma_node_is_leaf(node)) {
        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* child = node->slots[i];

            if (child && !mt_is_entry_ptr(child)) {
                destroy_subtree(static_cast<ma_node_t*>(child));
            }
        }
    }

    free_ma_node(node);
}

}

extern "C" void mt_init_cache(void) {
    if (!g_ma_node_cache) {
        g_ma_node_cache = kmem_cache_create("maple_node", sizeof(ma_node_t), 64u, 0u);
    }
}

extern "C" void* mt_load(maple_tree_t* mt, uint32_t index) {
    if (!mt
        || mt_empty(mt)) {
        return nullptr;
    }

    rcu_read_lock();

    void* root = rcu_ptr_read((const rcu_ptr_t*)&mt->ma_root);

    if (mt_is_entry_ptr(root)) {
        void* raw_entry = mt_entry_to_slot(root);

        uint32_t entry_end = *((uint32_t*)raw_entry + 1);

        if (index < entry_end) {
            rcu_read_unlock();

            return raw_entry;
        }

        rcu_read_unlock();

        return nullptr;
    }

    ma_node_t* node = static_cast<ma_node_t*>(root);

    while (node && !ma_node_is_leaf(node)) {
        bool descended = false;

        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* child = ma_slot_rcu(node, i);

            if (!child) {
                continue;
            }

            uint32_t pivot = (i < MT_PIVOT_COUNT) ? node->pivots[i] : kMaxKey;

            if (index <= pivot) {
                node = static_cast<ma_node_t*>(child);
                descended = true;
                break;
            }
        }

        if (!descended) {
            break;
        }
    }

    void* result = nullptr;

    if (node && ma_node_is_leaf(node)) {
        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            if (!node->slots[i]) {
                continue;
            }

            void* raw_entry = mt_entry_to_slot(node->slots[i]);
            uint32_t entry_end = *((uint32_t*)raw_entry + 1);

            if (index < entry_end) {
                result = raw_entry;
                break;
            }
        }
    }

    rcu_read_unlock();

    return result;
}

extern "C" int mt_store(maple_tree_t* mt, uint32_t index, uint32_t last, void* entry) {
    if (!mt
        || !entry) {
        return -1;
    }

    spinlock_acquire(&mt->ma_lock);

    if (mt_empty(mt)) {
        ma_node_t* leaf = alloc_ma_node(MT_NODE_LEAF);

        if (!leaf) {
            spinlock_release(&mt->ma_lock);
            return -1;
        }

        rcu_ptr_assign((rcu_ptr_t*)&leaf->slots[0], mt_slot_to_entry(entry));
        leaf->pivots[0] = last;

        for (uint8_t i = 1u; i < MT_SLOT_COUNT; i++) {
            leaf->slots[i] = nullptr;
        }

        for (uint8_t i = 1u; i < MT_PIVOT_COUNT; i++) {
            leaf->pivots[i] = 0u;
        }

        rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, leaf);

        spinlock_release(&mt->ma_lock);

        return 0;
    }

    void* root = mt->ma_root;

    if (mt_is_entry_ptr(root)) {
        ma_node_t* leaf = alloc_ma_node(MT_NODE_LEAF);

        if (!leaf) {
            spinlock_release(&mt->ma_lock);
            return -1;
        }

        rcu_ptr_assign((rcu_ptr_t*)&leaf->slots[0], root);
        leaf->pivots[0] = *((uint32_t*)mt_entry_to_slot(root) + 1) - 1u;

        rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, leaf);
    }

    ma_state_t mas{};

    mas.tree = mt;
    mas.index = index;
    mas.last = last;
    mas.node = static_cast<ma_node_t*>(mt->ma_root);

    if (!ma_descend(&mas, index)) {
        spinlock_release(&mt->ma_lock);
        return -1;
    }

    ma_node_t* leaf = mas.node;

    uint8_t offset = MT_SLOT_COUNT;

    for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
        if (!leaf->slots[i]) {
            offset = i;

            break;
        }

        uint32_t e_start = *(uint32_t*)mt_entry_to_slot(leaf->slots[i]);

        if (e_start == index) {
            rcu_ptr_assign((rcu_ptr_t*)&leaf->slots[i], mt_slot_to_entry(entry));

            if (i < MT_PIVOT_COUNT) {
                leaf->pivots[i] = last;
            }

            ma_propagate_pivots(mt, leaf);

            spinlock_release(&mt->ma_lock);

            return 0;
        }

        if (e_start > index) {
            offset = i;

            break;
        }
    }

    if (slot_count(leaf) < MT_SLOT_COUNT) {
        if (offset >= MT_SLOT_COUNT) {
            spinlock_release(&mt->ma_lock);

            return -1;
        }

        shift_slots_right(leaf, offset, 1u);
        shift_pivots_right(leaf, offset, 1u);

        rcu_ptr_assign((rcu_ptr_t*)&leaf->slots[offset], mt_slot_to_entry(entry));

        if (offset < MT_PIVOT_COUNT) {
            leaf->pivots[offset] = last;
        }

        ma_propagate_pivots(mt, leaf);

        spinlock_release(&mt->ma_lock);

        return 0;
    }

    ma_node_t* parent = ma_parent(leaf);

    uint8_t pslot = ma_parent_slot(leaf);

    if (split_node(mt, leaf, parent, pslot) < 0) {
        spinlock_release(&mt->ma_lock);
        return -1;
    }

    mas.node = static_cast<ma_node_t*>(mt->ma_root);

    if (!ma_descend(&mas, index)) {
        spinlock_release(&mt->ma_lock);
        return -1;
    }

    leaf = mas.node;

    offset = MT_SLOT_COUNT;

    for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
        if (!leaf->slots[i]) {
            offset = i;

            break;
        }

        uint32_t e_start = *(uint32_t*)mt_entry_to_slot(leaf->slots[i]);

        if (e_start > index) {
            offset = i;

            break;
        }
    }

    if (slot_count(leaf) < MT_SLOT_COUNT && offset < MT_SLOT_COUNT) {
        shift_slots_right(leaf, offset, 1u);
        shift_pivots_right(leaf, offset, 1u);

        rcu_ptr_assign((rcu_ptr_t*)&leaf->slots[offset], mt_slot_to_entry(entry));

        if (offset < MT_PIVOT_COUNT) {
            leaf->pivots[offset] = last;
        }

        ma_propagate_pivots(mt, leaf);
    }

    spinlock_release(&mt->ma_lock);

    return 0;
}

extern "C" int mt_erase(maple_tree_t* mt, uint32_t index, uint32_t last) {
    if (!mt
        || mt_empty(mt)) {
        return -1;
    }

    spinlock_acquire(&mt->ma_lock);

    void* root = mt->ma_root;

    if (mt_is_entry_ptr(root)) {
        uint32_t e_start = *(uint32_t*)mt_entry_to_slot(root);

        if (e_start == index) {
            rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, (void*)MA_ROOT_FLAG);

            spinlock_release(&mt->ma_lock);

            return 0;
        }

        spinlock_release(&mt->ma_lock);

        return -1;
    }

    ma_state_t mas{};

    mas.tree = mt;
    mas.index = index;
    mas.last = last;
    mas.node = static_cast<ma_node_t*>(root);

    if (!ma_descend(&mas, index)) {
        spinlock_release(&mt->ma_lock);
        return -1;
    }

    ma_node_t* leaf = mas.node;

    uint8_t offset = MT_SLOT_COUNT;

    for (uint8_t i = 0; i < MT_SLOT_COUNT; i++) {
        if (!leaf->slots[i]) {
            continue;
        }

        uint32_t e_start = *(uint32_t*)mt_entry_to_slot(leaf->slots[i]);

        if (e_start == index) {
            offset = i;

            break;
        }
    }

    if (offset == MT_SLOT_COUNT) {
        spinlock_release(&mt->ma_lock);
        return -1;
    }

    rcu_ptr_assign((rcu_ptr_t*)&leaf->slots[offset], nullptr);

    shift_slots_left(leaf, offset, 1u);
    shift_pivots_left(leaf, offset, 1u);

    if (slot_count(leaf) == 0u) {
        ma_node_t* parent = ma_parent(leaf);

        uint8_t pslot = ma_parent_slot(leaf);

        if (parent) {
            rcu_ptr_assign((rcu_ptr_t*)&parent->slots[pslot], nullptr);

            shift_slots_left(parent, pslot, 1u);
            shift_pivots_left(parent, pslot, 1u);

            for (uint8_t i = pslot; i < MT_SLOT_COUNT; i++) {
                if (parent->slots[i]) {
                    ma_node_t* sibling = static_cast<ma_node_t*>(ma_slot_rcu(parent, i));

                    ma_set_parent(sibling, parent, i);
                }
            }

            call_rcu((rcu_head_t*)&leaf->slots[0], free_ma_node_rcu);

            rebalance_after_erase(mt, parent);
        } else {
            rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, (void*)MA_ROOT_FLAG);

            call_rcu((rcu_head_t*)&leaf->slots[0], free_ma_node_rcu);
        }
    } else {
        ma_recalc_pivots(leaf);
        ma_propagate_pivots(mt, leaf);
    }

    spinlock_release(&mt->ma_lock);

    return 0;
}

extern "C" void* mt_find(maple_tree_t* mt, uint32_t* index, uint32_t max) {
    if (!mt
        || !index
        || mt_empty(mt)) {
        return nullptr;
    }

    uint32_t search_idx = *index;

    rcu_read_lock();

    void* root = rcu_ptr_read((const rcu_ptr_t*)&mt->ma_root);

    if (mt_is_entry_ptr(root)) {
        void* raw_entry = mt_entry_to_slot(root);

        uint32_t e_start = *(uint32_t*)raw_entry;
        uint32_t e_end = *((uint32_t*)raw_entry + 1);

        if (search_idx < e_end && e_start <= max) {
            *index = e_end - 1u;

            rcu_read_unlock();

            return raw_entry;
        }

        rcu_read_unlock();

        return nullptr;
    }

    ma_node_t* node = static_cast<ma_node_t*>(root);

    while (node && !ma_node_is_leaf(node)) {
        bool descended = false;

        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* child = ma_slot_rcu(node, i);

            if (!child) {
                continue;
            }

            uint32_t pivot = (i < MT_PIVOT_COUNT) ? node->pivots[i] : kMaxKey;

            if (search_idx <= pivot) {
                node = static_cast<ma_node_t*>(child);
                descended = true;
                break;
            }
        }

        if (!descended) {
            break;
        }
    }

    void* result = nullptr;

    while (node && ma_node_is_leaf(node)) {
        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            if (!node->slots[i]) {
                continue;
            }

            void* raw_entry = mt_entry_to_slot(node->slots[i]);

            uint32_t e_start = *(uint32_t*)raw_entry;
            uint32_t e_end = *((uint32_t*)raw_entry + 1);

            if (search_idx < e_end) {
                if (e_start > max) {
                    rcu_read_unlock();
                    return nullptr;
                }

                result = raw_entry;
                *index = e_end - 1u;

                rcu_read_unlock();
                return result;
            }
        }

        ma_node_t* parent = ma_parent(node);

        uint8_t slot = ma_parent_slot(node);

        ma_node_t* next_leaf = nullptr;

        while (parent) {
            slot++;

            if (slot < MT_SLOT_COUNT && parent->slots[slot]) {
                ma_node_t* child = static_cast<ma_node_t*>(ma_slot_rcu(parent, slot));

                while (child && !ma_node_is_leaf(child)) {
                    child = static_cast<ma_node_t*>(ma_slot_rcu(child, 0u));
                }

                next_leaf = child;

                break;
            }

            slot = ma_parent_slot(parent);

            parent = ma_parent(parent);
        }

        node = next_leaf;
    }

    rcu_read_unlock();

    return result;
}

extern "C" void* mt_find_after(maple_tree_t* mt, uint32_t index) {
    uint32_t idx = index;

    return mt_find(mt, &idx, kMaxKey);
}

extern "C" int mt_next(maple_tree_t* mt, uint32_t* index) {
    if (!mt
        || !index
        || mt_empty(mt)) {
        return 0;
    }

    uint32_t search_idx = *index + 1u;

    if (search_idx == 0u) {
        return 0;
    }

    rcu_read_lock();

    void* root = rcu_ptr_read((const rcu_ptr_t*)&mt->ma_root);

    if (mt_is_entry_ptr(root)) {
        rcu_read_unlock();
        return 0;
    }

    ma_node_t* node = static_cast<ma_node_t*>(root);

    while (node && !ma_node_is_leaf(node)) {
        bool descended = false;

        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* child = ma_slot_rcu(node, i);

            if (!child) {
                continue;
            }

            uint32_t pivot = (i < MT_PIVOT_COUNT) ? node->pivots[i] : kMaxKey;

            if (search_idx <= pivot) {
                node = static_cast<ma_node_t*>(child);
                descended = true;
                break;
            }
        }

        if (!descended) {
            break;
        }
    }

    int found = 0;

    while (node && ma_node_is_leaf(node)) {
        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            if (!node->slots[i]) {
                continue;
            }

            void* raw_entry = mt_entry_to_slot(node->slots[i]);
            uint32_t entry_end = *((uint32_t*)raw_entry + 1);
            uint32_t entry_last = entry_end - 1u;

            if (search_idx <= entry_last) {
                *index = entry_last;
                found = 1;

                rcu_read_unlock();
                return found;
            }
        }

        ma_node_t* parent = ma_parent(node);

        uint8_t slot = ma_parent_slot(node);

        ma_node_t* next_leaf = nullptr;

        while (parent) {
            slot++;

            if (slot < MT_SLOT_COUNT && parent->slots[slot]) {
                ma_node_t* child = static_cast<ma_node_t*>(ma_slot_rcu(parent, slot));

                while (child && !ma_node_is_leaf(child)) {
                    child = static_cast<ma_node_t*>(ma_slot_rcu(child, 0u));
                }

                next_leaf = child;

                break;
            }

            slot = ma_parent_slot(parent);

            parent = ma_parent(parent);
        }

        node = next_leaf;
    }

    rcu_read_unlock();

    return found;
}

extern "C" int mt_prev(maple_tree_t* mt, uint32_t* index) {
    if (!mt
        || !index
        || *index == 0u
        || mt_empty(mt)) {
        return 0;
    }

    uint32_t search_idx = *index - 1u;

    rcu_read_lock();

    void* root = rcu_ptr_read((const rcu_ptr_t*)&mt->ma_root);

    if (mt_is_entry_ptr(root)) {
        rcu_read_unlock();
        return 0;
    }

    ma_node_t* node = static_cast<ma_node_t*>(root);

    while (node && !ma_node_is_leaf(node)) {
        bool descended = false;

        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* child = ma_slot_rcu(node, i);

            if (!child) {
                continue;
            }

            uint32_t pivot = (i < MT_PIVOT_COUNT) ? node->pivots[i] : kMaxKey;

            if (search_idx <= pivot) {
                node = static_cast<ma_node_t*>(child);
                descended = true;
                break;
            }
        }

        if (!descended) {
            break;
        }
    }

    int found = 0;

    while (node && ma_node_is_leaf(node)) {
        for (uint8_t i = MT_SLOT_COUNT; i > 0u; i--) {
            uint8_t idx = i - 1u;

            if (node->slots[idx]) {
                void* raw_entry = mt_entry_to_slot(node->slots[idx]);

                uint32_t entry_end = *((uint32_t*)raw_entry + 1);
                uint32_t entry_last = entry_end - 1u;

                if (entry_last <= search_idx) {
                    *index = entry_last;
                    found = 1;

                    rcu_read_unlock();
                    return found;
                }
            }
        }

        ma_node_t* parent = ma_parent(node);

        uint8_t slot = ma_parent_slot(node);

        ma_node_t* prev_leaf = nullptr;

        while (parent) {
            if (slot > 0u && parent->slots[slot - 1u]) {
                ma_node_t* child = static_cast<ma_node_t*>(ma_slot_rcu(parent, slot - 1u));

                while (child && !ma_node_is_leaf(child)) {
                    uint8_t last_slot = 0;

                    for (uint8_t k = 0u; k < MT_SLOT_COUNT; k++) {
                        if (!child->slots[k]) {
                            break;
                        }

                        last_slot = k;
                    }

                    child = static_cast<ma_node_t*>(ma_slot_rcu(child, last_slot));
                }

                prev_leaf = child;

                break;
            }

            slot = ma_parent_slot(parent);

            parent = ma_parent(parent);
        }

        node = prev_leaf;
    }

    rcu_read_unlock();

    return found;
}

extern "C" int mt_gap_find(
    maple_tree_t* mt, uint32_t size,
    uint32_t* out_index, uint32_t floor,
    uint32_t ceiling
) {
    if (!mt
        || !out_index
        || size == 0u) {
        return 0;
    }

    rcu_read_lock();

    if (mt_empty(mt)) {
        if (ceiling > floor && ceiling - floor >= size) {
            *out_index = floor;

            rcu_read_unlock();
            return 1;
        }

        rcu_read_unlock();
        return 0;
    }

    void* root = rcu_ptr_read((const rcu_ptr_t*)&mt->ma_root);

    if (mt_is_entry_ptr(root)) {
        void* raw_entry = mt_entry_to_slot(root);
        uint32_t e_start = *(uint32_t*)raw_entry;
        uint32_t e_end = *((uint32_t*)raw_entry + 1);

        if (e_start > floor) {
            uint32_t gap_end = (e_start < ceiling) ? e_start : ceiling;

            if (gap_end > floor && gap_end - floor >= size) {
                *out_index = floor;

                rcu_read_unlock();
                return 1;
            }
        }

        if (e_end >= floor && ceiling > e_end && ceiling - e_end >= size) {
            *out_index = (e_end > floor) ? e_end : floor;

            rcu_read_unlock();
            return 1;
        }

        rcu_read_unlock();
        return 0;
    }

    ma_node_t* node = static_cast<ma_node_t*>(root);

    while (node && !ma_node_is_leaf(node)) {
        bool descended = false;

        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* child = ma_slot_rcu(node, i);

            if (!child) {
                continue;
            }

            uint32_t pivot = (i < MT_PIVOT_COUNT) ? node->pivots[i] : kMaxKey;

            if (floor <= pivot) {
                node = static_cast<ma_node_t*>(child);
                descended = true;
                break;
            }
        }

        if (!descended) {
            break;
        }
    }

    uint32_t current_start = floor;

    while (node && ma_node_is_leaf(node)) {
        for (uint8_t i = 0u; i < MT_SLOT_COUNT; i++) {
            void* entry = node->slots[i];

            if (!entry) {
                continue;
            }

            uint32_t entry_start = *(uint32_t*)mt_entry_to_slot(entry);
            uint32_t entry_end = *((uint32_t*)mt_entry_to_slot(entry) + 1);

            if (entry_start > current_start) {
                uint32_t gap_start = current_start;
                uint32_t gap_end = (entry_start < ceiling) ? entry_start : ceiling;

                if (gap_end > gap_start && gap_end - gap_start >= size) {
                    *out_index = gap_start;

                    rcu_read_unlock();
                    return 1;
                }
            }

            if (entry_end > current_start) {
                current_start = entry_end;
            }
        }

        ma_node_t* parent = ma_parent(node);

        uint8_t slot = ma_parent_slot(node);

        ma_node_t* next_leaf = nullptr;

        while (parent) {
            slot++;

            if (slot < MT_SLOT_COUNT && parent->slots[slot]) {
                ma_node_t* child = static_cast<ma_node_t*>(ma_slot_rcu(parent, slot));

                while (child && !ma_node_is_leaf(child)) {
                    child = static_cast<ma_node_t*>(ma_slot_rcu(child, 0u));
                }

                next_leaf = child;

                break;
            }

            slot = ma_parent_slot(parent);

            parent = ma_parent(parent);
        }

        node = next_leaf;
    }

    if (ceiling > current_start && ceiling - current_start >= size) {
        *out_index = current_start;

        rcu_read_unlock();
        return 1;
    }

    rcu_read_unlock();

    return 0;
}

extern "C" void mt_destroy(maple_tree_t* mt) {
    if (!mt) {
        return;
    }

    spinlock_acquire(&mt->ma_lock);

    void* root = mt->ma_root;

    if (root == (void*)MA_ROOT_FLAG
        || root == nullptr) {
        spinlock_release(&mt->ma_lock);
        return;
    }

    if (!mt_is_entry_ptr(root)) {
        ma_node_t* node = static_cast<ma_node_t*>(root);

        destroy_subtree(node);
    }

    rcu_ptr_assign((rcu_ptr_t*)&mt->ma_root, (void*)MA_ROOT_FLAG);

    spinlock_release(&mt->ma_lock);
}
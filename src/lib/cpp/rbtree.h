#ifndef LIB_CPP_RBTREE_H
#define LIB_CPP_RBTREE_H

#include <lib/rbtree.h>

#include <stddef.h>
#include <stdint.h>

namespace kernel {

namespace detail {

constexpr inline void rb_reset_node(rb_node& node) noexcept {
    node.__parent_color = 0;
    node.rb_left = nullptr;
    node.rb_right = nullptr;
}

template <class T, size_t Offset>
struct RbMemberHook {
    static_assert(Offset < sizeof(T), "RbMemberHook offset out of bounds");

    static rb_node* node_ptr(T* value) noexcept {
        if (!value) {
            return nullptr;
        }

        return reinterpret_cast<rb_node*>(reinterpret_cast<uintptr_t>(value) + Offset);
    }

    static const rb_node* node_ptr(const T* value) noexcept {
        if (!value) {
            return nullptr;
        }

        return reinterpret_cast<const rb_node*>(reinterpret_cast<uintptr_t>(value) + Offset);
    }

    static T* value_ptr(rb_node* node) noexcept {
        if (!node) {
            return nullptr;
        }

        return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(node) - Offset);
    }

    static const T* value_ptr(const rb_node* node) noexcept {
        if (!node) {
            return nullptr;
        }

        return reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(node) - Offset);
    }

    static void reset(T& value) noexcept {
        rb_node* node = node_ptr(&value);
        if (!node) {
            return;
        }

        rb_reset_node(*node);
    }
};

}

template <class T>
struct RbDefaultCompare {
    bool operator()(const T& a, const T& b) const {
        return a < b;
    }
};

template <class T>
struct RbIdentityKeyOfValue {
    const T& operator()(const T& value) const {
        return value;
    }
};

template <class Key>
struct RbDefaultLess {
    bool operator()(const Key& a, const Key& b) const {
        return a < b;
    }
};

template <
    class T,
    class Hook,
    class Key = T,
    class KeyOfValue = RbIdentityKeyOfValue<T>,
    class CompareKey = RbDefaultLess<Key>
>
class IntrusiveRbTree {
public:
    using value_type = T;
    using key_type = Key;

    class iterator;
    class const_iterator;

    struct range {
        iterator first;
        iterator second;
    };

    struct const_range {
        const_iterator first;
        const_iterator second;
    };

    class iterator {
    public:
        using value_type = T;

        iterator() = default;

        T& operator*() const {
            return *value_;
        }

        T* operator->() const {
            return value_;
        }

        iterator& operator++() {
            if (!value_) {
                return *this;
            }

            rb_node* next = rb_next(Hook::node_ptr(value_));
            value_ = Hook::value_ptr(next);
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        iterator& operator--() {
            if (!value_) {
                return *this;
            }

            rb_node* prev = rb_prev(Hook::node_ptr(value_));
            value_ = Hook::value_ptr(prev);
            return *this;
        }

        iterator operator--(int) {
            iterator tmp = *this;
            --(*this);
            return tmp;
        }

        friend bool operator==(const iterator& a, const iterator& b) {
            return a.value_ == b.value_;
        }

        friend bool operator!=(const iterator& a, const iterator& b) {
            return a.value_ != b.value_;
        }

    private:
        friend class IntrusiveRbTree;

        explicit iterator(T* v)
            : value_(v) {
        }

        T* value_ = nullptr;
    };

    class const_iterator {
    public:
        using value_type = const T;

        const_iterator() = default;

        const T& operator*() const {
            return *value_;
        }

        const T* operator->() const {
            return value_;
        }

        const_iterator& operator++() {
            if (!value_) {
                return *this;
            }

            rb_node* next = rb_next(Hook::node_ptr(value_));
            value_ = Hook::value_ptr(next);
            return *this;
        }

        const_iterator operator++(int) {
            const_iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        const_iterator& operator--() {
            if (!value_) {
                return *this;
            }

            rb_node* prev = rb_prev(Hook::node_ptr(value_));
            value_ = Hook::value_ptr(prev);
            return *this;
        }

        const_iterator operator--(int) {
            const_iterator tmp = *this;
            --(*this);
            return tmp;
        }

        friend bool operator==(const const_iterator& a, const const_iterator& b) {
            return a.value_ == b.value_;
        }

        friend bool operator!=(const const_iterator& a, const const_iterator& b) {
            return a.value_ != b.value_;
        }

    private:
        friend class IntrusiveRbTree;

        explicit const_iterator(const T* v)
            : value_(v) {
        }

        const T* value_ = nullptr;
    };

    IntrusiveRbTree() = default;

    IntrusiveRbTree(const IntrusiveRbTree&) = delete;
    IntrusiveRbTree& operator=(const IntrusiveRbTree&) = delete;

    void clear() noexcept {
        rb_node* node = rb_first(&root_);
        while (node) {
            rb_node* next = rb_next(node);

            rb_erase(node, &root_);
            detail::rb_reset_node(*node);

            node = next;
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return root_.rb_node == nullptr;
    }

    iterator begin() noexcept {
        return iterator(Hook::value_ptr(rb_first(&root_)));
    }

    iterator end() noexcept {
        return iterator(nullptr);
    }

    const_iterator begin() const noexcept {
        return const_iterator(Hook::value_ptr(rb_first(&root_)));
    }

    const_iterator end() const noexcept {
        return const_iterator(nullptr);
    }

    const_iterator cbegin() const noexcept {
        return begin();
    }

    const_iterator cend() const noexcept {
        return end();
    }

    iterator last() noexcept {
        return iterator(Hook::value_ptr(rb_last(&root_)));
    }

    const_iterator last() const noexcept {
        return const_iterator(Hook::value_ptr(rb_last(&root_)));
    }

    void erase(T& value) noexcept {
        rb_node* node = Hook::node_ptr(&value);

        rb_erase(node, &root_);
        detail::rb_reset_node(*node);
    }

    [[nodiscard]] bool insert_unique(T& value) noexcept {
        const key_type& value_key = key_of_value_(value);

        rb_node** link = &root_.rb_node;
        rb_node* parent = nullptr;

        while (*link) {
            T* curr = Hook::value_ptr(*link);
            parent = *link;

            const key_type& curr_key = key_of_value_(*curr);

            if (cmp_key_(value_key, curr_key)) {
                link = &((*link)->rb_left);
                continue;
            }

            if (cmp_key_(curr_key, value_key)) {
                link = &((*link)->rb_right);
                continue;
            }

            return false;
        }

        rb_node* node = Hook::node_ptr(&value);

        rb_link_node(node, parent, link);
        rb_insert_color(node, &root_);
        return true;
    }

    [[nodiscard]] iterator find(const key_type& key) noexcept {
        rb_node* node = root_.rb_node;

        while (node) {
            T* curr = Hook::value_ptr(node);
            const key_type& curr_key = key_of_value_(*curr);

            if (cmp_key_(key, curr_key)) {
                node = node->rb_left;
                continue;
            }

            if (cmp_key_(curr_key, key)) {
                node = node->rb_right;
                continue;
            }

            return iterator(curr);
        }

        return end();
    }

    [[nodiscard]] const_iterator find(const key_type& key) const noexcept {
        const rb_node* node = root_.rb_node;

        while (node) {
            const T* curr = Hook::value_ptr(node);
            const key_type& curr_key = key_of_value_(*curr);

            if (cmp_key_(key, curr_key)) {
                node = node->rb_left;
                continue;
            }

            if (cmp_key_(curr_key, key)) {
                node = node->rb_right;
                continue;
            }

            return const_iterator(curr);
        }

        return end();
    }

    [[nodiscard]] iterator lower_bound(const key_type& key) noexcept {
        rb_node* node = root_.rb_node;
        T* best = nullptr;

        while (node) {
            T* curr = Hook::value_ptr(node);
            const key_type& curr_key = key_of_value_(*curr);

            if (!cmp_key_(curr_key, key)) {
                best = curr;
                node = node->rb_left;
            } else {
                node = node->rb_right;
            }
        }

        return iterator(best);
    }

    [[nodiscard]] const_iterator lower_bound(const key_type& key) const noexcept {
        const rb_node* node = root_.rb_node;
        const T* best = nullptr;

        while (node) {
            const T* curr = Hook::value_ptr(node);
            const key_type& curr_key = key_of_value_(*curr);

            if (!cmp_key_(curr_key, key)) {
                best = curr;
                node = node->rb_left;
            } else {
                node = node->rb_right;
            }
        }

        return const_iterator(best);
    }

    [[nodiscard]] iterator upper_bound(const key_type& key) noexcept {
        rb_node* node = root_.rb_node;
        T* best = nullptr;

        while (node) {
            T* curr = Hook::value_ptr(node);
            const key_type& curr_key = key_of_value_(*curr);

            if (cmp_key_(key, curr_key)) {
                best = curr;
                node = node->rb_left;
            } else {
                node = node->rb_right;
            }
        }

        return iterator(best);
    }

    [[nodiscard]] const_iterator upper_bound(const key_type& key) const noexcept {
        const rb_node* node = root_.rb_node;
        const T* best = nullptr;

        while (node) {
            const T* curr = Hook::value_ptr(node);
            const key_type& curr_key = key_of_value_(*curr);

            if (cmp_key_(key, curr_key)) {
                best = curr;
                node = node->rb_left;
            } else {
                node = node->rb_right;
            }
        }

        return const_iterator(best);
    }

    [[nodiscard]] range equal_range(const key_type& key) noexcept {
        return range{lower_bound(key), upper_bound(key)};
    }

    [[nodiscard]] const_range equal_range(const key_type& key) const noexcept {
        return const_range{lower_bound(key), upper_bound(key)};
    }

    [[nodiscard]] iterator find_key(const key_type& key) noexcept {
        return find(key);
    }

    [[nodiscard]] const_iterator find_key(const key_type& key) const noexcept {
        return find(key);
    }

    [[nodiscard]] iterator lower_bound_key(const key_type& key) noexcept {
        return lower_bound(key);
    }

    [[nodiscard]] const_iterator lower_bound_key(const key_type& key) const noexcept {
        return lower_bound(key);
    }

    [[nodiscard]] iterator upper_bound_key(const key_type& key) noexcept {
        return upper_bound(key);
    }

    [[nodiscard]] const_iterator upper_bound_key(const key_type& key) const noexcept {
        return upper_bound(key);
    }

    rb_root* native_handle() noexcept {
        return &root_;
    }

    const rb_root* native_handle() const noexcept {
        return &root_;
    }

private:
    rb_root root_ = RB_ROOT;

    KeyOfValue key_of_value_{};
    CompareKey cmp_key_{};
};

}

#endif

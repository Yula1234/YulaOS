// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_CPP_DLIST_H
#define YOS_CPP_DLIST_H

#include <lib/dlist.h>

#include <lib/cpp/new.h>
#include <lib/cpp/utility.h>

namespace kernel {

template<typename T, dlist_head_t T::* Member>
class CDBLinkedList {
public:
    class iterator {
    public:
        iterator() = default;

        T& operator*() const {
            return *value_ptr_;
        }

        T* operator->() const {
            return value_ptr_;
        }

        iterator& operator++() {
            node_ = node_->next;
            value_ptr_ = value_ptr_from_node_(node_);

            return *this;
        }

        iterator operator++(int) {
            iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const iterator& other) const {
            return node_ == other.node_;
        }

        bool operator!=(const iterator& other) const {
            return !(*this == other);
        }

    private:
        friend class CDBLinkedList;

        iterator(dlist_head_t* node, dlist_head_t* head)
            : node_(node),
              head_(head),
              value_ptr_(value_ptr_from_node_(node)) {
        }

        T* value_ptr_from_node_(dlist_head_t* node) const {
            if (!node || node == head_) {
                return nullptr;
            }

            const size_t off = member_offset_();
            return reinterpret_cast<T*>(reinterpret_cast<char*>(node) - off);
        }

        static size_t member_offset_() {
            return reinterpret_cast<size_t>(&(reinterpret_cast<T*>(0)->*Member));
        }

        dlist_head_t* node_ = nullptr;
        dlist_head_t* head_ = nullptr;
        T* value_ptr_ = nullptr;
    };

    class const_iterator {
    public:
        const_iterator() = default;

        const T& operator*() const {
            return *value_ptr_;
        }

        const T* operator->() const {
            return value_ptr_;
        }

        const_iterator& operator++() {
            node_ = node_->next;
            value_ptr_ = value_ptr_from_node_(node_);

            return *this;
        }

        const_iterator operator++(int) {
            const_iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const const_iterator& other) const {
            return node_ == other.node_;
        }

        bool operator!=(const const_iterator& other) const {
            return !(*this == other);
        }

    private:
        friend class CDBLinkedList;

        const_iterator(const dlist_head_t* node, const dlist_head_t* head)
            : node_(node),
              head_(head),
              value_ptr_(value_ptr_from_node_(node)) {
        }

        const T* value_ptr_from_node_(const dlist_head_t* node) const {
            if (!node || node == head_) {
                return nullptr;
            }

            const size_t off = member_offset_();
            return reinterpret_cast<const T*>(reinterpret_cast<const char*>(node) - off);
        }

        static size_t member_offset_() {
            return reinterpret_cast<size_t>(&(reinterpret_cast<T*>(0)->*Member));
        }

        const dlist_head_t* node_ = nullptr;
        const dlist_head_t* head_ = nullptr;
        const T* value_ptr_ = nullptr;
    };

    using value_type = T;
    using reference = T&;
    using const_reference = const T&;

    CDBLinkedList() {
        dlist_init(&head_);
    }

    CDBLinkedList(const CDBLinkedList&) = delete;
    CDBLinkedList& operator=(const CDBLinkedList&) = delete;

    CDBLinkedList(CDBLinkedList&&) = delete;
    CDBLinkedList& operator=(CDBLinkedList&&) = delete;

    bool empty() const {
        return dlist_empty(&head_) != 0;
    }

    iterator begin() {
        return iterator(head_.next, &head_);
    }

    iterator end() {
        return iterator(&head_, &head_);
    }

    const_iterator begin() const {
        return const_iterator(head_.next, &head_);
    }

    const_iterator end() const {
        return const_iterator(&head_, &head_);
    }

    const_iterator cbegin() const {
        return begin();
    }

    const_iterator cend() const {
        return end();
    }

    reference front() {
        return *iterator(head_.next, &head_);
    }

    const_reference front() const {
        return *const_iterator(head_.next, &head_);
    }

    reference back() {
        return *iterator(head_.prev, &head_);
    }

    const_reference back() const {
        return *const_iterator(head_.prev, &head_);
    }

    void push_front(T& value) {
        dlist_add(&(value.*Member), &head_);
    }

    void push_back(T& value) {
        dlist_add_tail(&(value.*Member), &head_);
    }

    iterator erase(iterator it) {
        dlist_head_t* node = it.node_;
        dlist_head_t* next = node->next;

        dlist_del(node);

        return iterator(next, &head_);
    }

    void clear_links_unsafe() {
        dlist_init(&head_);
    }

    dlist_head_t* native_head() {
        return &head_;
    }

    const dlist_head_t* native_head() const {
        return &head_;
    }

private:
    dlist_head_t head_{};
};

template<typename T, dlist_head_t T::* Member>
class CDBLinkedListView {
public:
    class iterator {
    public:
        iterator() = default;

        T& operator*() const {
            return *value_ptr_;
        }

        T* operator->() const {
            return value_ptr_;
        }

        iterator& operator++() {
            node_ = node_->next;
            value_ptr_ = value_ptr_from_node_(node_);

            return *this;
        }

        iterator operator++(int) {
            iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const iterator& other) const {
            return node_ == other.node_;
        }

        bool operator!=(const iterator& other) const {
            return !(*this == other);
        }

    private:
        friend class CDBLinkedListView;

        iterator(dlist_head_t* node, dlist_head_t* head)
            : node_(node),
              head_(head),
              value_ptr_(value_ptr_from_node_(node)) {
        }

        T* value_ptr_from_node_(dlist_head_t* node) const {
            if (!node || node == head_) {
                return nullptr;
            }

            const size_t off = member_offset_();
            return reinterpret_cast<T*>(reinterpret_cast<char*>(node) - off);
        }

        static size_t member_offset_() {
            return reinterpret_cast<size_t>(&(reinterpret_cast<T*>(0)->*Member));
        }

        dlist_head_t* node_ = nullptr;
        dlist_head_t* head_ = nullptr;

        T* value_ptr_ = nullptr;
    };

    class const_iterator {
    public:
        const_iterator() = default;

        const T& operator*() const {
            return *value_ptr_;
        }

        const T* operator->() const {
            return value_ptr_;
        }

        const_iterator& operator++() {
            node_ = node_->next;
            value_ptr_ = value_ptr_from_node_(node_);

            return *this;
        }

        const_iterator operator++(int) {
            const_iterator copy = *this;
            ++(*this);
            return copy;
        }

        bool operator==(const const_iterator& other) const {
            return node_ == other.node_;
        }

        bool operator!=(const const_iterator& other) const {
            return !(*this == other);
        }

    private:
        friend class CDBLinkedListView;

        const_iterator(const dlist_head_t* node, const dlist_head_t* head)
            : node_(node),
              head_(head),
              value_ptr_(value_ptr_from_node_(node)) {
        }

        const T* value_ptr_from_node_(const dlist_head_t* node) const {
            if (!node || node == head_) {
                return nullptr;
            }

            const size_t off = member_offset_();
            return reinterpret_cast<const T*>(reinterpret_cast<const char*>(node) - off);
        }

        static size_t member_offset_() {
            return reinterpret_cast<size_t>(&(reinterpret_cast<T*>(0)->*Member));
        }

        const dlist_head_t* node_ = nullptr;
        const dlist_head_t* head_ = nullptr;
        const T* value_ptr_ = nullptr;
    };

    using value_type = T;
    using reference = T&;
    using const_reference = const T&;

    explicit CDBLinkedListView(dlist_head_t& head)
        : head_(&head) {
    }

    bool empty() const {
        return dlist_empty(head_) != 0;
    }

    iterator begin() {
        return iterator(head_->next, head_);
    }

    iterator end() {
        return iterator(head_, head_);
    }

    const_iterator begin() const {
        return const_iterator(head_->next, head_);
    }

    const_iterator end() const {
        return const_iterator(head_, head_);
    }

    const_iterator cbegin() const {
        return begin();
    }

    const_iterator cend() const {
        return end();
    }

    reference front() {
        return *iterator(head_->next, head_);
    }

    const_reference front() const {
        return *const_iterator(head_->next, head_);
    }

    reference back() {
        return *iterator(head_->prev, head_);
    }

    const_reference back() const {
        return *const_iterator(head_->prev, head_);
    }

    void push_front(T& value) {
        dlist_add(&(value.*Member), head_);
    }

    void push_back(T& value) {
        dlist_add_tail(&(value.*Member), head_);
    }

    iterator erase(iterator it) {
        dlist_head_t* node = it.node_;
        dlist_head_t* next = node->next;

        dlist_del(node);

        return iterator(next, head_);
    }

    dlist_head_t& native_head() {
        return *head_;
    }

    const dlist_head_t& native_head() const {
        return *head_;
    }

private:
    dlist_head_t* head_ = nullptr;
};

class DBLinkedListBase {
public:
    using CreateCopyFn = void* (*)(const void* value);
    using CreateMoveFn = void* (*)(void* value);

    using DestroyFn = void (*)(void* value);
    
    using MoveOutFn = void (*)(void* dst, void* src);
    using CompareFn = bool (*)(const void* a, const void* b);

    DBLinkedListBase(CreateCopyFn create_copy_fn,
                     CreateMoveFn create_move_fn,
                     DestroyFn destroy_fn,
                     MoveOutFn move_out_fn,
                     CompareFn compare_fn);
    ~DBLinkedListBase();

    DBLinkedListBase(const DBLinkedListBase&) = delete;
    DBLinkedListBase& operator=(const DBLinkedListBase&) = delete;

    DBLinkedListBase(DBLinkedListBase&&) = delete;
    DBLinkedListBase& operator=(DBLinkedListBase&&) = delete;

    bool push_back_copy(const void* value);
    bool push_back_move(void* value);

    bool push_front_copy(const void* value);
    bool push_front_move(void* value);
    
    bool pop_front(void* out);
    bool pop_back(void* out);
    
    bool remove(const void* value);
    bool empty() const;

private:
    struct Node {
        Node* prev;
        Node* next;
        void* payload;
    };

    CreateCopyFn m_create_copy;
    CreateMoveFn m_create_move;

    DestroyFn m_destroy;
    MoveOutFn m_move_out;
    CompareFn m_compare;
    
    Node* m_head;
    Node* m_tail;
};

template<typename T>
class DBLinkedList {
public:
    DBLinkedList()
        : m_base(&DBLinkedList::create_copy,
                 &DBLinkedList::create_move,
                 &DBLinkedList::destroy,
                 &DBLinkedList::move_out,
                 &DBLinkedList::compare) {
    }

    ~DBLinkedList() {
    }

    DBLinkedList(const DBLinkedList&) = delete;
    DBLinkedList& operator=(const DBLinkedList&) = delete;

    DBLinkedList(DBLinkedList&&) = delete;
    DBLinkedList& operator=(DBLinkedList&&) = delete;

    bool push_back(const T& value) {
        return m_base.push_back_copy(&value);
    }

    bool push_back(T&& value) {
        return m_base.push_back_move(&value);
    }

    bool push_front(const T& value) {
        return m_base.push_front_copy(&value);
    }

    bool push_front(T&& value) {
        return m_base.push_front_move(&value);
    }

    bool pop_front(T& out) {
        return m_base.pop_front(&out);
    }

    bool pop_back(T& out) {
        return m_base.pop_back(&out);
    }

    bool remove(const T& value) {
        return m_base.remove(&value);
    }

    bool empty() const {
        if (m_base.empty()) {
            return true;
        }
        return false;
    }

private:
    template<typename U>
    static auto create_copy_impl(const void* value, int)
        -> decltype(U(*static_cast<const U*>(value)), (void*)0) {
        const U* src = static_cast<const U*>(value);
        return new (kernel::nothrow) U(*src);
    }

    template<typename U>
    static void* create_copy_impl(const void*, long) {
        return nullptr;
    }

    static void* create_copy(const void* value) {
        return create_copy_impl<T>(value, 0);
    }

    static void* create_move(void* value) {
        T* src = (T*)value;
        return new (kernel::nothrow) T(kernel::move(*src));
    }

    static void destroy(void* value) {
        T* obj = (T*)value;
        delete obj;
    }

    static void move_out(void* dst, void* src) {
        T* out = (T*)dst;
        T* in = (T*)src;
        *out = kernel::move(*in);
    }

    static bool compare(const void* a, const void* b) {
        const T* lhs = (const T*)a;
        const T* rhs = (const T*)b;

        return (*lhs == *rhs);
    }

    DBLinkedListBase m_base;
};

inline bool DBLinkedListBase::empty() const {
    if (m_head == nullptr) {
        return true;
    }
    
    return false;
}

} // namespace kernel

#endif

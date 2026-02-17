// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_CPP_DLIST_H
#define YOS_CPP_DLIST_H

#include <lib/cpp/new.h>
#include <lib/cpp/utility.h>

namespace kernel {

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

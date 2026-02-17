// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_CPP_DLIST_H
#define YOS_CPP_DLIST_H

#include <lib/cpp/new.h>

namespace kernel {

class DBLinkedListBase {
public:
    using CreateFn = void* (*)(const void* value);
    using DestroyFn = void (*)(void* value);
    using CopyOutFn = void (*)(void* dst, const void* src);
    using CompareFn = bool (*)(const void* a, const void* b);

    DBLinkedListBase(CreateFn create_fn,
                     DestroyFn destroy_fn,
                     CopyOutFn copy_out_fn,
                     CompareFn compare_fn);
    ~DBLinkedListBase();

    DBLinkedListBase(const DBLinkedListBase&) = delete;
    DBLinkedListBase& operator=(const DBLinkedListBase&) = delete;
    DBLinkedListBase(DBLinkedListBase&&) = delete;
    DBLinkedListBase& operator=(DBLinkedListBase&&) = delete;

    bool push_back(const void* value);
    bool push_front(const void* value);
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

    CreateFn m_create;
    DestroyFn m_destroy;
    CopyOutFn m_copy_out;
    CompareFn m_compare;
    Node* m_head;
    Node* m_tail;
};

template<typename T>
class DBLinkedList {
public:
    DBLinkedList()
        : m_base(&DBLinkedList::create,
                 &DBLinkedList::destroy,
                 &DBLinkedList::copy_out,
                 &DBLinkedList::compare) {
    }

    ~DBLinkedList() {
    }

    DBLinkedList(const DBLinkedList&) = delete;
    DBLinkedList& operator=(const DBLinkedList&) = delete;
    DBLinkedList(DBLinkedList&&) = delete;
    DBLinkedList& operator=(DBLinkedList&&) = delete;

    bool push_back(const T& value) {
        return m_base.push_back(&value);
    }

    bool push_front(const T& value) {
        return m_base.push_front(&value);
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
    static void* create(const void* value) {
        const T* src = (const T*)value;
        return new (kernel::nothrow) T(*src);
    }

    static void destroy(void* value) {
        T* obj = (T*)value;
        delete obj;
    }

    static void copy_out(void* dst, const void* src) {
        T* out = (T*)dst;
        const T* in = (const T*)src;
        *out = *in;
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

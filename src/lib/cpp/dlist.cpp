// SPDX-License-Identifier: GPL-2.0

#include <lib/cpp/dlist.h>

namespace kernel {

DBLinkedListBase::DBLinkedListBase(CreateCopyFn create_copy_fn,
                                   CreateMoveFn create_move_fn,
                                   DestroyFn destroy_fn,
                                   MoveOutFn move_out_fn,
                                   CompareFn compare_fn)
    : m_create_copy(create_copy_fn),
      m_create_move(create_move_fn),
      m_destroy(destroy_fn),
      m_move_out(move_out_fn),
      m_compare(compare_fn),
      m_head(nullptr),
      m_tail(nullptr) {
}

DBLinkedListBase::~DBLinkedListBase() {
    Node* n = m_head;
    while (n) {
        Node* next = n->next;
        if (m_destroy && n->payload) {
            m_destroy(n->payload);
        }
        delete n;
        n = next;
    }
    m_head = nullptr;
    m_tail = nullptr;
}

bool DBLinkedListBase::push_back_copy(const void* value) {
    if (!m_create_copy) {
        return false;
    }

    void* payload = m_create_copy(value);
    if (!payload) {
        return false;
    }

    Node* node = new (kernel::nothrow) Node;
    if (!node) {
        if (m_destroy) {
            m_destroy(payload);
        }
        return false;
    }

    node->prev = m_tail;
    node->next = nullptr;
    node->payload = payload;

    if (m_tail) {
        m_tail->next = node;
    } else {
        m_head = node;
    }
    m_tail = node;
    return true;
}

bool DBLinkedListBase::push_back_move(void* value) {
    if (!m_create_move) {
        return false;
    }

    void* payload = m_create_move(value);
    if (!payload) {
        return false;
    }

    Node* node = new (kernel::nothrow) Node;
    if (!node) {
        if (m_destroy) {
            m_destroy(payload);
        }
        return false;
    }

    node->prev = m_tail;
    node->next = nullptr;
    node->payload = payload;

    if (m_tail) {
        m_tail->next = node;
    } else {
        m_head = node;
    }
    m_tail = node;
    return true;
}

bool DBLinkedListBase::push_front_copy(const void* value) {
    if (!m_create_copy) {
        return false;
    }

    void* payload = m_create_copy(value);
    if (!payload) {
        return false;
    }

    Node* node = new (kernel::nothrow) Node;
    if (!node) {
        if (m_destroy) {
            m_destroy(payload);
        }
        return false;
    }

    node->prev = nullptr;
    node->next = m_head;
    node->payload = payload;

    if (m_head) {
        m_head->prev = node;
    } else {
        m_tail = node;
    }
    m_head = node;
    return true;
}

bool DBLinkedListBase::push_front_move(void* value) {
    if (!m_create_move) {
        return false;
    }

    void* payload = m_create_move(value);
    if (!payload) {
        return false;
    }

    Node* node = new (kernel::nothrow) Node;
    if (!node) {
        if (m_destroy) {
            m_destroy(payload);
        }
        return false;
    }

    node->prev = nullptr;
    node->next = m_head;
    node->payload = payload;

    if (m_head) {
        m_head->prev = node;
    } else {
        m_tail = node;
    }
    m_head = node;
    return true;
}

bool DBLinkedListBase::pop_front(void* out) {
    if (!out || !m_move_out) {
        return false;
    }

    Node* n = m_head;
    if (!n) {
        return false;
    }

    Node* next = n->next;
    m_head = next;
    if (next) {
        next->prev = nullptr;
    } else {
        m_tail = nullptr;
    }

    if (n->payload) {
        m_move_out(out, n->payload);
        if (m_destroy) {
            m_destroy(n->payload);
        }
    }
    delete n;
    return true;
}

bool DBLinkedListBase::pop_back(void* out) {
    if (!out || !m_move_out) {
        return false;
    }

    Node* n = m_tail;
    if (!n) {
        return false;
    }

    Node* prev = n->prev;
    m_tail = prev;
    if (prev) {
        prev->next = nullptr;
    } else {
        m_head = nullptr;
    }

    if (n->payload) {
        m_move_out(out, n->payload);
        if (m_destroy) {
            m_destroy(n->payload);
        }
    }
    delete n;
    return true;
}

bool DBLinkedListBase::remove(const void* value) {
    if (!m_compare) {
        return false;
    }

    Node* cur = m_head;
    while (cur) {
        if (m_compare(cur->payload, value)) {
            break;
        }
        cur = cur->next;
    }
    if (!cur) {
        return false;
    }

    Node* prev = cur->prev;
    Node* next = cur->next;

    if (prev) {
        prev->next = next;
    } else {
        m_head = next;
    }

    if (next) {
        next->prev = prev;
    } else {
        m_tail = prev;
    }

    if (m_destroy && cur->payload) {
        m_destroy(cur->payload);
    }
    delete cur;
    return true;
}

} // namespace kernel

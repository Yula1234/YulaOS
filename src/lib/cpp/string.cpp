// SPDX-License-Identifier: GPL-2.0

#include <lib/cpp/string.h>

#include <lib/cpp/new.h>
#include <lib/cpp/utility.h>

namespace kernel {

string::string() {
    init_inline();
}

string::string(const char* s) {
    init_inline();
    (void)assign(s);
}

string::string(const char* s, size_t len) {
    init_inline();
    (void)assign(s, len);
}

string::string(const string& other) {
    init_inline();
    (void)assign(other);
}

string::string(string&& other) noexcept {
    move_from(kernel::move(other));
}

string::~string() {
    destroy();
}

string& string::operator=(const string& other) {
    if (this == &other) {
        return *this;
    }
    (void)assign(other);
    return *this;
}

string& string::operator=(string&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroy();
    move_from(kernel::move(other));
    return *this;
}

bool string::assign(const char* s) {
    if (!s) {
        clear();
        return true;
    }
    return assign(s, strlen(s));
}

bool string::assign(const char* s, size_t len) {
    if (!s) {
        if (len == 0u) {
            clear();
            return true;
        }
        return false;
    }

    if (s == m_data && len == m_size) {
        return true;
    }

    if (len <= m_capacity) {
        memmove(m_data, s, len);
        m_data[len] = 0;
        m_size = len;
        return true;
    }

    const size_t new_cap = recommend_capacity(len);
    char* new_buf = new (kernel::nothrow) char[new_cap + 1u];
    if (!new_buf) {
        return false;
    }

    memcpy(new_buf, s, len);
    new_buf[len] = 0;

    if (!is_inline()) {
        delete[] m_data;
    }

    m_data = new_buf;
    m_capacity = new_cap;
    m_size = len;
    return true;
}

bool string::assign(const string& other) {
    if (this == &other) {
        return true;
    }
    return assign(other.data(), other.size());
}

bool string::append(const char* s) {
    if (!s) {
        return false;
    }
    return append(s, strlen(s));
}

bool string::append(const char* s, size_t len) {
    if (len == 0u) {
        return true;
    }
    if (!s) {
        return false;
    }

    const size_t new_size = m_size + len;
    const bool overlap = (s >= m_data && s < (m_data + m_size));

    if (new_size <= m_capacity) {
        memmove(m_data + m_size, s, len);
        m_size = new_size;
        m_data[m_size] = 0;
        return true;
    }

    const size_t new_cap = recommend_capacity(new_size);
    char* new_buf = new (kernel::nothrow) char[new_cap + 1u];
    if (!new_buf) {
        return false;
    }

    if (m_size > 0u) {
        memcpy(new_buf, m_data, m_size);
    }

    if (overlap) {
        const size_t offset = (size_t)(s - m_data);
        memmove(new_buf + m_size, new_buf + offset, len);
    } else {
        memcpy(new_buf + m_size, s, len);
    }

    new_buf[new_size] = 0;

    if (!is_inline()) {
        delete[] m_data;
    }

    m_data = new_buf;
    m_capacity = new_cap;
    m_size = new_size;
    return true;
}

bool string::append(const string& other) {
    return append(other.data(), other.size());
}

bool string::push_back(char c) {
    return append(&c, 1u);
}

void string::clear() {
    m_size = 0u;
    m_data[0] = 0;
}

bool string::reserve(size_t new_cap) {
    if (new_cap <= m_capacity) {
        return true;
    }
    return grow(new_cap);
}

void string::shrink_to_fit() {
    if (m_size <= inline_capacity) {
        if (is_inline()) {
            return;
        }

        char* old = m_data;
        memcpy(m_inline, old, m_size);
        m_inline[m_size] = 0;
        m_data = m_inline;
        m_capacity = inline_capacity;
        delete[] old;
        return;
    }

    if (m_capacity == m_size) {
        return;
    }

    char* new_buf = new (kernel::nothrow) char[m_size + 1u];
    if (!new_buf) {
        return;
    }

    memcpy(new_buf, m_data, m_size);
    new_buf[m_size] = 0;

    if (!is_inline()) {
        delete[] m_data;
    }

    m_data = new_buf;
    m_capacity = m_size;
}

bool string::operator==(const string& other) const {
    if (m_size != other.m_size) {
        return false;
    }
    if (m_size == 0u) {
        return true;
    }
    return memcmp(m_data, other.m_data, m_size) == 0;
}

bool string::operator!=(const string& other) const {
    return !(*this == other);
}

uint32_t string::hash() const {
    uint32_t h = 5381u;
    for (size_t i = 0u; i < m_size; i++) {
        h = ((h << 5) + h) + (uint32_t)m_data[i];
    }
    return h;
}

bool string::is_inline() const {
    return m_data == m_inline;
}

void string::init_inline() {
    m_data = m_inline;
    m_size = 0u;
    m_capacity = inline_capacity;
    m_inline[0] = 0;
}

void string::destroy() {
    if (!is_inline()) {
        delete[] m_data;
    }
    m_data = nullptr;
    m_size = 0u;
    m_capacity = 0u;
}

bool string::grow(size_t min_capacity) {
    if (min_capacity <= m_capacity) {
        return true;
    }

    const size_t new_cap = recommend_capacity(min_capacity);
    char* new_buf = new (kernel::nothrow) char[new_cap + 1u];
    if (!new_buf) {
        return false;
    }

    if (m_size > 0u) {
        memcpy(new_buf, m_data, m_size);
    }
    new_buf[m_size] = 0;

    if (!is_inline()) {
        delete[] m_data;
    }

    m_data = new_buf;
    m_capacity = new_cap;
    return true;
}

size_t string::recommend_capacity(size_t min_capacity) const {
    size_t cap = m_capacity;
    if (cap < inline_capacity) {
        cap = inline_capacity;
    }

    while (cap < min_capacity) {
        const size_t next = cap * 2u;
        if (next < cap) {
            return min_capacity;
        }
        cap = next;
    }

    return cap;
}

void string::move_from(string&& other) {
    if (other.is_inline()) {
        init_inline();
        if (other.m_size > 0u) {
            memcpy(m_inline, other.m_inline, other.m_size);
            m_inline[other.m_size] = 0;
            m_size = other.m_size;
        }
        other.clear();
        return;
    }

    m_data = other.m_data;
    m_size = other.m_size;
    m_capacity = other.m_capacity;

    other.init_inline();
}

}

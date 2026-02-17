// SPDX-License-Identifier: GPL-2.0

#ifndef LIB_CPP_STRING_H
#define LIB_CPP_STRING_H

#include <stddef.h>
#include <stdint.h>

#include <lib/string.h>

namespace kernel {

class string {
public:
    static constexpr size_t inline_capacity = 31u;

    string();
    explicit string(const char* s);
    string(const char* s, size_t len);
    string(const string& other);
    string(string&& other) noexcept;
    ~string();

    string& operator=(const string& other);
    string& operator=(string&& other) noexcept;

    bool assign(const char* s);
    bool assign(const char* s, size_t len);
    bool assign(const string& other);

    bool append(const char* s);
    bool append(const char* s, size_t len);
    bool append(const string& other);

    bool push_back(char c);

    void clear();
    bool reserve(size_t new_cap);
    void shrink_to_fit();

    size_t size() const {
        return m_size;
    }

    size_t capacity() const {
        return m_capacity;
    }

    bool empty() const {
        return m_size == 0u;
    }

    const char* c_str() const {
        return m_data;
    }

    const char* data() const {
        return m_data;
    }

    char* data() {
        return m_data;
    }

    bool operator==(const string& other) const;
    bool operator!=(const string& other) const;

    uint32_t hash() const;

private:
    bool is_inline() const;
    void init_inline();
    void destroy();
    bool grow(size_t min_capacity);
    size_t recommend_capacity(size_t min_capacity) const;
    void move_from(string&& other);

    char* m_data;
    size_t m_size;
    size_t m_capacity;
    char m_inline[inline_capacity + 1u];
};

}

#endif

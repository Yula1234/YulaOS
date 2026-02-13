#ifndef YOS_NETD_VEC_H
#define YOS_NETD_VEC_H

#include "arena.h"

#include <stdint.h>
#include <stddef.h>

#include <lib/string.h>

#include <new>

namespace netd {

template <typename T>
static inline T&& move(T& v) {
    return (T&&)v;
}

template <typename T>
class Vector {
public:
    Vector() : m_arena(nullptr), m_data(nullptr), m_size(0), m_capacity(0) {
    }

    explicit Vector(Arena& arena) : m_arena(&arena), m_data(nullptr), m_size(0), m_capacity(0) {
    }

    Vector(const Vector&) = delete;
    Vector& operator=(const Vector&) = delete;

    ~Vector() {
        destroy_range(0, m_size);
    }

    void bind(Arena& arena) {
        m_arena = &arena;
    }

    uint32_t size() const {
        return m_size;
    }

    uint32_t capacity() const {
        return m_capacity;
    }

    T* data() {
        return m_data;
    }

    const T* data() const {
        return m_data;
    }

    T& operator[](uint32_t i) {
        return m_data[i];
    }

    const T& operator[](uint32_t i) const {
        return m_data[i];
    }

    T* begin() {
        return m_data;
    }

    T* end() {
        return m_data + m_size;
    }

    const T* begin() const {
        return m_data;
    }

    const T* end() const {
        return m_data + m_size;
    }

    bool reserve(uint32_t new_capacity) {
        if (new_capacity <= m_capacity) {
            return true;
        }

        if (!m_arena) {
            return false;
        }

        const uint32_t bytes = new_capacity * (uint32_t)sizeof(T);
        void* p = m_arena->alloc(bytes, (uint32_t)alignof(T));
        if (!p) {
            return false;
        }

        T* new_data = (T*)p;

        for (uint32_t i = 0; i < m_size; i++) {
            new (&new_data[i]) T(move(m_data[i]));
        }

        destroy_range(0, m_size);

        m_data = new_data;
        m_capacity = new_capacity;
        return true;
    }

    bool push_back(const T& v) {
        if (m_size >= m_capacity) {
            uint32_t new_capacity = m_capacity ? m_capacity * 2u : 4u;
            if (new_capacity < (m_size + 1u)) {
                new_capacity = m_size + 1u;
            }

            if (!reserve(new_capacity)) {
                return false;
            }
        }

        new (&m_data[m_size]) T(v);
        m_size++;
        return true;
    }

    bool push_back(T&& v) {
        if (m_size >= m_capacity) {
            uint32_t new_capacity = m_capacity ? m_capacity * 2u : 4u;
            if (new_capacity < (m_size + 1u)) {
                new_capacity = m_size + 1u;
            }

            if (!reserve(new_capacity)) {
                return false;
            }
        }

        new (&m_data[m_size]) T(move(v));
        m_size++;
        return true;
    }

    void erase_unordered(uint32_t i) {
        if (i >= m_size) {
            return;
        }

        const uint32_t last = m_size - 1u;
        if (i != last) {
            m_data[i] = move(m_data[last]);
        }

        m_data[last].~T();
        m_size--;
    }

    void clear() {
        destroy_range(0, m_size);
        m_size = 0;
    }

private:
    void destroy_range(uint32_t first, uint32_t last) {
        if (!m_data) {
            return;
        }

        for (uint32_t i = first; i < last; i++) {
            m_data[i].~T();
        }
    }

    Arena* m_arena;
    T* m_data;
    uint32_t m_size;
    uint32_t m_capacity;
};

}

#endif

#ifndef YOS_NETD_VEC_H
#define YOS_NETD_VEC_H

#include "arena.h"

#include <stdint.h>
#include <stddef.h>

#include <lib/string.h>

namespace netd {

template <typename T>
class Vector {
public:
    Vector() : m_arena(nullptr), m_data(nullptr), m_size(0), m_capacity(0) {
    }

    explicit Vector(Arena& arena) : m_arena(&arena), m_data(nullptr), m_size(0), m_capacity(0) {
    }

    Vector(const Vector&) = delete;
    Vector& operator=(const Vector&) = delete;

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

        if (m_data && m_size > 0) {
            memcpy(new_data, m_data, (uint32_t)(m_size * sizeof(T)));
        }

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

        m_data[m_size] = v;
        m_size++;
        return true;
    }

    void erase_unordered(uint32_t i) {
        if (i >= m_size) {
            return;
        }

        m_size--;
        if (i != m_size) {
            m_data[i] = m_data[m_size];
        }
    }

    void clear() {
        m_size = 0;
    }

private:
    Arena* m_arena;
    T* m_data;
    uint32_t m_size;
    uint32_t m_capacity;
};

}

#endif

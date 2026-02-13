#ifndef YOS_NETD_HASH_MAP_H
#define YOS_NETD_HASH_MAP_H

#include "arena.h"

#include <stdint.h>

namespace netd {

template <typename K>
struct DefaultHashTraits;

template <typename K, typename V, typename Traits = DefaultHashTraits<K>>
class HashMap {
public:
    HashMap() : m_arena(nullptr), m_keys(nullptr), m_vals(nullptr), m_capacity(0), m_size(0) {
    }

    explicit HashMap(Arena& arena)
        : m_arena(&arena),
          m_keys(nullptr),
          m_vals(nullptr),
          m_capacity(0),
          m_size(0) {
    }

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    void bind(Arena& arena) {
        m_arena = &arena;
    }

    uint32_t size() const {
        return m_size;
    }

    uint32_t capacity() const {
        return m_capacity;
    }

    bool reserve(uint32_t min_capacity) {
        uint32_t need = 1u;
        while (need < min_capacity) {
            need <<= 1u;
        }

        if (need <= m_capacity) {
            return true;
        }

        if (!m_arena) {
            return false;
        }

        K* new_keys = (K*)m_arena->alloc((uint32_t)(need * sizeof(K)), (uint32_t)alignof(K));
        V* new_vals = (V*)m_arena->alloc((uint32_t)(need * sizeof(V)), (uint32_t)alignof(V));
        if (!new_keys || !new_vals) {
            return false;
        }

        const K empty = Traits::empty_key();
        for (uint32_t i = 0; i < need; i++) {
            new_keys[i] = empty;
            new_vals[i] = V{};
        }

        if (m_capacity != 0) {
            for (uint32_t i = 0; i < m_capacity; i++) {
                const K k = m_keys[i];
                if (Traits::is_empty(k)) {
                    continue;
                }

                (void)insert_into(new_keys, new_vals, need, k, m_vals[i]);
            }
        }

        m_keys = new_keys;
        m_vals = new_vals;
        m_capacity = need;
        return true;
    }

    bool put(const K& key, const V& val) {
        if (Traits::is_empty(key)) {
            return false;
        }

        if ((m_size + 1u) * 10u >= m_capacity * 7u) {
            const uint32_t next = m_capacity ? (m_capacity * 2u) : 16u;
            if (!reserve(next)) {
                return false;
            }
        }

        if (m_capacity == 0) {
            if (!reserve(16u)) {
                return false;
            }
        }

        bool is_new = false;
        const uint32_t idx = find_slot(m_keys, m_capacity, key, is_new);
        if (idx == 0xFFFFFFFFu) {
            return false;
        }

        if (is_new) {
            m_keys[idx] = key;
            m_vals[idx] = val;
            m_size++;
            return true;
        }

        m_vals[idx] = val;
        return true;
    }

    bool get(const K& key, V& out_val) const {
        if (Traits::is_empty(key) || m_capacity == 0) {
            return false;
        }

        const uint32_t mask = m_capacity - 1u;
        uint32_t i = Traits::hash(key) & mask;

        for (uint32_t probes = 0; probes < m_capacity; probes++) {
            const K k = m_keys[i];
            if (Traits::is_empty(k)) {
                return false;
            }

            if (Traits::eq(k, key)) {
                out_val = m_vals[i];
                return true;
            }

            i = (i + 1u) & mask;
        }

        return false;
    }

    bool erase(const K& key) {
        if (Traits::is_empty(key) || m_capacity == 0) {
            return false;
        }

        const uint32_t mask = m_capacity - 1u;
        uint32_t i = Traits::hash(key) & mask;

        for (uint32_t probes = 0; probes < m_capacity; probes++) {
            const K k = m_keys[i];
            if (Traits::is_empty(k)) {
                return false;
            }

            if (Traits::eq(k, key)) {
                m_keys[i] = Traits::empty_key();
                m_vals[i] = V{};

                if (m_size) {
                    m_size--;
                }

                shift_cluster(i);
                return true;
            }

            i = (i + 1u) & mask;
        }

        return false;
    }

private:
    static uint32_t find_slot(K* keys, uint32_t cap, const K& key, bool& out_is_new) {
        const uint32_t mask = cap - 1u;
        uint32_t i = Traits::hash(key) & mask;

        for (uint32_t probes = 0; probes < cap; probes++) {
            const K k = keys[i];
            if (Traits::is_empty(k)) {
                out_is_new = true;
                return i;
            }

            if (Traits::eq(k, key)) {
                out_is_new = false;
                return i;
            }

            i = (i + 1u) & mask;
        }

        return 0xFFFFFFFFu;
    }

    static bool insert_into(K* keys, V* vals, uint32_t cap, const K& key, const V& val) {
        bool is_new = false;
        const uint32_t idx = find_slot(keys, cap, key, is_new);
        if (idx == 0xFFFFFFFFu) {
            return false;
        }

        keys[idx] = key;
        vals[idx] = val;
        return true;
    }

    bool relocate_no_size(const K& key, const V& val) {
        if (Traits::is_empty(key) || m_capacity == 0) {
            return false;
        }

        bool is_new = false;
        const uint32_t idx = find_slot(m_keys, m_capacity, key, is_new);
        if (idx == 0xFFFFFFFFu) {
            return false;
        }

        m_keys[idx] = key;
        m_vals[idx] = val;
        return true;
    }

    void shift_cluster(uint32_t empty_idx) {
        if (m_capacity == 0) {
            return;
        }

        const uint32_t mask = m_capacity - 1u;
        uint32_t i = (empty_idx + 1u) & mask;

        while (!Traits::is_empty(m_keys[i])) {
            const K k = m_keys[i];
            const V v = m_vals[i];

            m_keys[i] = Traits::empty_key();
            m_vals[i] = V{};

            (void)relocate_no_size(k, v);

            i = (i + 1u) & mask;
        }
    }

    Arena* m_arena;
    K* m_keys;
    V* m_vals;
    uint32_t m_capacity;
    uint32_t m_size;
};

template <>
struct DefaultHashTraits<uint32_t> {
    static constexpr uint32_t empty_key() {
        return 0u;
    }

    static constexpr bool is_empty(uint32_t k) {
        return k == 0u;
    }

    static uint32_t hash(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    static constexpr bool eq(uint32_t a, uint32_t b) {
        return a == b;
    }
};

}

#endif

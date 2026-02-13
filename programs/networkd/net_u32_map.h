#ifndef YOS_NETD_U32_MAP_H
#define YOS_NETD_U32_MAP_H

#include "arena.h"

#include <stdint.h>

namespace netd {

class U32Map {
public:
    U32Map() : m_arena(nullptr), m_keys(nullptr), m_vals(nullptr), m_capacity(0), m_size(0) {
    }

    explicit U32Map(Arena& arena) : m_arena(&arena), m_keys(nullptr), m_vals(nullptr), m_capacity(0), m_size(0) {
    }

    U32Map(const U32Map&) = delete;
    U32Map& operator=(const U32Map&) = delete;

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

        uint32_t* new_keys = (uint32_t*)m_arena->alloc((uint32_t)(need * sizeof(uint32_t)), 4u);
        uint32_t* new_vals = (uint32_t*)m_arena->alloc((uint32_t)(need * sizeof(uint32_t)), 4u);
        if (!new_keys || !new_vals) {
            return false;
        }

        for (uint32_t i = 0; i < need; i++) {
            new_keys[i] = 0u;
            new_vals[i] = 0u;
        }

        if (m_capacity != 0) {
            for (uint32_t i = 0; i < m_capacity; i++) {
                const uint32_t k = m_keys[i];
                if (k == 0u) {
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

    bool put(uint32_t key, uint32_t val) {
        if (key == 0u) {
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

    bool relocate_no_size(uint32_t key, uint32_t val) {
        if (key == 0u || m_capacity == 0) {
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

    bool get(uint32_t key, uint32_t& out_val) const {
        if (key == 0u || m_capacity == 0) {
            return false;
        }

        const uint32_t mask = m_capacity - 1u;
        uint32_t i = hash(key) & mask;

        for (uint32_t probes = 0; probes < m_capacity; probes++) {
            const uint32_t k = m_keys[i];
            if (k == 0u) {
                return false;
            }

            if (k == key) {
                out_val = m_vals[i];
                return true;
            }

            i = (i + 1u) & mask;
        }

        return false;
    }

    bool erase(uint32_t key) {
        if (key == 0u || m_capacity == 0) {
            return false;
        }

        const uint32_t mask = m_capacity - 1u;
        uint32_t i = hash(key) & mask;

        for (uint32_t probes = 0; probes < m_capacity; probes++) {
            const uint32_t k = m_keys[i];
            if (k == 0u) {
                return false;
            }

            if (k == key) {
                m_keys[i] = 0u;
                m_vals[i] = 0u;

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
    static uint32_t hash(uint32_t x) {
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    static uint32_t find_slot(uint32_t* keys, uint32_t cap, uint32_t key, bool& out_is_new) {
        const uint32_t mask = cap - 1u;
        uint32_t i = hash(key) & mask;

        for (uint32_t probes = 0; probes < cap; probes++) {
            const uint32_t k = keys[i];
            if (k == 0u) {
                out_is_new = true;
                return i;
            }

            if (k == key) {
                out_is_new = false;
                return i;
            }

            i = (i + 1u) & mask;
        }

        return 0xFFFFFFFFu;
    }

    static bool insert_into(uint32_t* keys, uint32_t* vals, uint32_t cap, uint32_t key, uint32_t val) {
        bool is_new = false;
        const uint32_t idx = find_slot(keys, cap, key, is_new);
        if (idx == 0xFFFFFFFFu) {
            return false;
        }

        keys[idx] = key;
        vals[idx] = val;
        return true;
    }

    bool put_no_grow(uint32_t key, uint32_t val) {
        if (key == 0u || m_capacity == 0) {
            return false;
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

    void shift_cluster(uint32_t empty_idx) {
        if (m_capacity == 0) {
            return;
        }

        const uint32_t mask = m_capacity - 1u;
        uint32_t i = (empty_idx + 1u) & mask;

        while (m_keys[i] != 0u) {
            const uint32_t k = m_keys[i];
            const uint32_t v = m_vals[i];

            m_keys[i] = 0u;
            m_vals[i] = 0u;

            (void)relocate_no_size(k, v);

            i = (i + 1u) & mask;
        }
    }

    Arena* m_arena;
    uint32_t* m_keys;
    uint32_t* m_vals;
    uint32_t m_capacity;
    uint32_t m_size;
};

}

#endif

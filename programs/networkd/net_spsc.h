#ifndef YOS_NETD_SPSC_H
#define YOS_NETD_SPSC_H

#include <stdint.h>

namespace netd {

template <typename T, uint32_t CapPow2>
class SpscQueue {
public:
    SpscQueue() : m_head(0), m_tail(0) {
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool push(const T& v) {
        const uint32_t head = m_head;
        const uint32_t next = head + 1u;

        if ((next - m_tail) > CapPow2) {
            return false;
        }

        m_buf[head & (CapPow2 - 1u)] = v;

        __sync_synchronize();
        m_head = next;
        return true;
    }

    bool pop(T& out) {
        const uint32_t tail = m_tail;
        if (tail == m_head) {
            return false;
        }

        out = m_buf[tail & (CapPow2 - 1u)];

        __sync_synchronize();
        m_tail = tail + 1u;
        return true;
    }

    uint32_t size_approx() const {
        return m_head - m_tail;
    }

private:
    static_assert((CapPow2 & (CapPow2 - 1u)) == 0u, "CapPow2 must be power of two");

    volatile uint32_t m_head;
    volatile uint32_t m_tail;
    T m_buf[CapPow2];
};

}

#endif

#ifndef YOS_NETD_CHANNEL_H
#define YOS_NETD_CHANNEL_H

#include "net_core.h"
#include "net_spsc.h"

namespace netd {

template <typename T, uint32_t CapPow2>
class SpscChannel {
public:
    SpscChannel(SpscQueue<T, CapPow2>& q, PipePair& notify) : m_q(q), m_notify(notify) {
    }

    SpscChannel(const SpscChannel&) = delete;
    SpscChannel& operator=(const SpscChannel&) = delete;

    bool push(const T& v) {
        return m_q.push(v);
    }

    bool push_and_wake(const T& v) {
        if (!m_q.push(v)) {
            return false;
        }

        m_notify.signal();
        return true;
    }

    bool pop(T& out) {
        return m_q.pop(out);
    }

    int notify_fd() const {
        return m_notify.read_fd();
    }

    void drain_notify() const {
        m_notify.drain();
    }

private:
    SpscQueue<T, CapPow2>& m_q;
    PipePair& m_notify;
};

}

#endif

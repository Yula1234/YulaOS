#include "netd_tick_scheduler.h"

namespace netd {

NetdTickScheduler::NetdTickScheduler(uint32_t poll_cap_ms) : m_poll_cap_ms(poll_cap_ms) {
}

int NetdTickScheduler::compute_poll_timeout_ms(uint32_t now_ms, uint32_t next_wakeup_ms) const {
    int timeout_ms = (int)m_poll_cap_ms;

    if (next_wakeup_ms == 0u) {
        return timeout_ms;
    }

    if (next_wakeup_ms <= now_ms) {
        return 0;
    }

    const uint32_t dt = next_wakeup_ms - now_ms;
    if (dt < m_poll_cap_ms) {
        timeout_ms = (int)dt;
    }

    return timeout_ms;
}

}

#ifndef YOS_NETD_TICK_SCHEDULER_H
#define YOS_NETD_TICK_SCHEDULER_H

#include <stdint.h>

namespace netd {

class NetdTickScheduler {
public:
    explicit NetdTickScheduler(uint32_t poll_cap_ms);

    NetdTickScheduler(const NetdTickScheduler&) = delete;
    NetdTickScheduler& operator=(const NetdTickScheduler&) = delete;

    int compute_poll_timeout_ms(uint32_t now_ms, uint32_t next_wakeup_ms) const;

private:
    uint32_t m_poll_cap_ms;
};

}

#endif

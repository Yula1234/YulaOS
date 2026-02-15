#ifndef YOS_NETD_TICK_SCHEDULER_H
#define YOS_NETD_TICK_SCHEDULER_H

#include "timing_wheel.h"
#include "arena.h"

#include <stdint.h>

namespace netd {

class NetdTickScheduler {
public:
    NetdTickScheduler(Arena& arena, uint32_t poll_cap_ms);

    NetdTickScheduler(const NetdTickScheduler&) = delete;
    NetdTickScheduler& operator=(const NetdTickScheduler&) = delete;

    bool init(uint32_t now_ms);

    TimerId schedule(uint32_t delay_ms, void* ctx, Timer::CallbackFn fn, uint32_t now_ms);
    TimerId schedule_at(uint32_t expires_at_ms, void* ctx, Timer::CallbackFn fn, uint32_t now_ms);
    
    bool cancel(TimerId timer_id);

    void tick(uint32_t now_ms);

    int compute_poll_timeout_ms(uint32_t now_ms, uint32_t next_wakeup_ms) const;

    uint32_t timer_count() const;
    uint32_t capacity() const;

    TimingWheel& wheel() {
        return m_wheel;
    }

    const TimingWheel& wheel() const {
        return m_wheel;
    }

private:
    TimingWheel m_wheel;
    uint32_t m_poll_cap_ms;
};

}

#endif

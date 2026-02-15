#include "netd_tick_scheduler.h"

namespace netd {

NetdTickScheduler::NetdTickScheduler(Arena& arena, uint32_t poll_cap_ms)
    : m_wheel(arena),
      m_poll_cap_ms(poll_cap_ms) {
}

bool NetdTickScheduler::init(uint32_t now_ms) {
    return m_wheel.init(now_ms);
}

TimerId NetdTickScheduler::schedule(
    uint32_t delay_ms,
    void* ctx,
    Timer::CallbackFn fn,
    uint32_t now_ms
) {
    return m_wheel.schedule(delay_ms, ctx, fn, now_ms);
}

TimerId NetdTickScheduler::schedule_at(
    uint32_t expires_at_ms,
    void* ctx,
    Timer::CallbackFn fn,
    uint32_t now_ms
) {
    return m_wheel.schedule_at(expires_at_ms, ctx, fn, now_ms);
}

bool NetdTickScheduler::cancel(TimerId timer_id) {
    return m_wheel.cancel(timer_id);
}

void NetdTickScheduler::tick(uint32_t now_ms) {
    m_wheel.tick(now_ms);
}

uint32_t NetdTickScheduler::timer_count() const {
    return m_wheel.timer_count();
}

uint32_t NetdTickScheduler::capacity() const {
    return m_wheel.capacity();
}

int NetdTickScheduler::compute_poll_timeout_ms(uint32_t now_ms, uint32_t next_wakeup_ms) const {
    int timeout_ms = (int)m_poll_cap_ms;

    if (m_wheel.has_pending_timers()) {
        timeout_ms = 1;
    }

    if (next_wakeup_ms == 0u) {
        return timeout_ms;
    }

    if (next_wakeup_ms <= now_ms) {
        return 0;
    }

    const uint32_t dt = next_wakeup_ms - now_ms;
    if (dt < (uint32_t)timeout_ms) {
        timeout_ms = (int)dt;
    }

    return timeout_ms;
}

}

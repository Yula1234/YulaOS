#include "timing_wheel.h"

#include <yula.h>

namespace netd {

TimingWheel::TimingWheel(Arena& arena)
    : m_arena(arena),
      m_wheels(),
      m_timer_pool(nullptr),
      m_pool_size(0),
      m_free_list(nullptr),
      m_free_count(0),
      m_active_count(0),
      m_current_time_ms(0),
      m_next_generation(1) {
}

bool TimingWheel::init(uint32_t now_ms) {
    const uint32_t pool_bytes = kMaxTimers * (uint32_t)sizeof(Timer);
    void* pool_mem = m_arena.alloc(pool_bytes, (uint32_t)alignof(Timer));
    if (!pool_mem) {
        return false;
    }

    m_timer_pool = (Timer*)pool_mem;
    m_pool_size = kMaxTimers;

    for (uint32_t i = 0; i < kMaxTimers; i++) {
        Timer* t = &m_timer_pool[i];
        memset(t, 0, sizeof(Timer));
        
        t->tw_next = m_free_list;
        m_free_list = t;
    }
    m_free_count = kMaxTimers;

    m_wheels[0].granularity_ms = kWheel0Granularity;
    m_wheels[1].granularity_ms = kWheel1Granularity;
    m_wheels[2].granularity_ms = kWheel2Granularity;
    m_wheels[3].granularity_ms = kWheel3Granularity;

    for (uint32_t w = 0; w < kWheelCount; w++) {
        m_wheels[w].current_slot = 0;
    }

    m_current_time_ms = now_ms;
    return true;
}

Timer* TimingWheel::alloc_timer() {
    if (!m_free_list) {
        return nullptr;
    }

    Timer* timer = m_free_list;
    m_free_list = timer->tw_next;

    if (m_free_count > 0) {
        m_free_count--;
    }

    memset(timer, 0, sizeof(Timer));
    timer->generation = m_next_generation++;
    if (m_next_generation == 0) {
        m_next_generation = 1;
    }

    return timer;
}

void TimingWheel::free_timer(Timer* timer) {
    if (!timer) {
        return;
    }

    timer->flags = 0;
    timer->tw_prev = nullptr;
    timer->tw_next = m_free_list;
    m_free_list = timer;
    m_free_count++;
}

void TimingWheel::link_timer(Timer* timer, uint8_t wheel_idx, uint8_t slot_idx) {
    if (!timer || wheel_idx >= kWheelCount) {
        return;
    }

    Wheel& wheel = m_wheels[wheel_idx];
    Slot& slot = wheel.slots[slot_idx];

    timer->wheel_index = wheel_idx;
    timer->slot_index = slot_idx;
    timer->flags |= Timer::FLAG_SCHEDULED;

    timer->tw_prev = nullptr;
    timer->tw_next = slot.head;

    if (slot.head) {
        slot.head->tw_prev = timer;
    } else {
        slot.tail = timer;
    }

    slot.head = timer;
}

void TimingWheel::unlink_timer(Timer* timer) {
    if (!timer || !(timer->flags & Timer::FLAG_SCHEDULED)) {
        return;
    }

    const uint8_t wheel_idx = timer->wheel_index;
    const uint8_t slot_idx = timer->slot_index;

    if (wheel_idx >= kWheelCount) {
        return;
    }

    Wheel& wheel = m_wheels[wheel_idx];
    Slot& slot = wheel.slots[slot_idx];

    if (timer->tw_prev) {
        timer->tw_prev->tw_next = timer->tw_next;
    } else {
        slot.head = timer->tw_next;
    }

    if (timer->tw_next) {
        timer->tw_next->tw_prev = timer->tw_prev;
    } else {
        slot.tail = timer->tw_prev;
    }

    timer->tw_prev = nullptr;
    timer->tw_next = nullptr;
    timer->flags &= ~Timer::FLAG_SCHEDULED;
}

uint32_t TimingWheel::compute_delay_ms(uint32_t expires_at_ms, uint32_t now_ms) const {
    if (expires_at_ms <= now_ms) {
        return 0;
    }

    return expires_at_ms - now_ms;
}

void TimingWheel::determine_wheel_and_slot(
    uint32_t delay_ms,
    uint8_t& out_wheel,
    uint8_t& out_slot
) const {
    const uint32_t slot_mask = kSlotsPerWheel - 1u;
    
    if (delay_ms < kWheel1Granularity) {
        out_wheel = 0;
        out_slot = (uint8_t)(delay_ms & slot_mask);
        return;
    }

    if (delay_ms < kWheel2Granularity) {
        out_wheel = 1;
        out_slot = (uint8_t)((delay_ms >> (kBitsPerWheel * 1)) & slot_mask);
        return;
    }

    if (delay_ms < kWheel3Granularity) {
        out_wheel = 2;
        out_slot = (uint8_t)((delay_ms >> (kBitsPerWheel * 2)) & slot_mask);
        return;
    }

    out_wheel = 3;
    out_slot = (uint8_t)((delay_ms >> (kBitsPerWheel * 3)) & slot_mask);
}

TimerId TimingWheel::schedule(
    uint32_t delay_ms,
    void* ctx,
    Timer::CallbackFn fn,
    uint32_t now_ms
) {
    const uint32_t expires_at_ms = now_ms + delay_ms;
    return schedule_at(expires_at_ms, ctx, fn, now_ms);
}

TimerId TimingWheel::schedule_at(
    uint32_t expires_at_ms,
    void* ctx,
    Timer::CallbackFn fn,
    uint32_t now_ms
) {
    if (!fn) {
        return TimerId();
    }

    Timer* timer = alloc_timer();
    if (!timer) {
        return TimerId();
    }

    timer->callback_ctx = ctx;
    timer->callback_fn = fn;
    timer->expires_at_ms = expires_at_ms;

    const uint32_t delay_ms = compute_delay_ms(expires_at_ms, now_ms);

    uint8_t wheel_idx = 0;
    uint8_t slot_idx = 0;
    determine_wheel_and_slot(delay_ms, wheel_idx, slot_idx);

    const uint32_t base_slot = m_wheels[wheel_idx].current_slot;
    const uint32_t slot_mask = kSlotsPerWheel - 1u;
    slot_idx = (uint8_t)((base_slot + slot_idx) & slot_mask);

    link_timer(timer, wheel_idx, slot_idx);

    m_active_count++;

    const uint32_t timer_index = (uint32_t)(timer - m_timer_pool);
    return TimerId(timer_index, timer->generation);
}

bool TimingWheel::cancel(TimerId timer_id) {
    if (!timer_id.is_valid() || timer_id.index >= m_pool_size) {
        return false;
    }

    Timer* timer = &m_timer_pool[timer_id.index];

    if (timer->generation != timer_id.generation) {
        return false;
    }

    if (!(timer->flags & Timer::FLAG_SCHEDULED)) {
        return false;
    }

    if (timer->flags & Timer::FLAG_CANCELLED) {
        return false;
    }

    timer->flags |= Timer::FLAG_CANCELLED;
    unlink_timer(timer);

    if (m_active_count > 0) {
        m_active_count--;
    }

    free_timer(timer);
    return true;
}

void TimingWheel::fire_timer(Timer* timer, uint32_t now_ms) {
    if (!timer || !timer->callback_fn) {
        return;
    }

    if (timer->flags & Timer::FLAG_CANCELLED) {
        return;
    }

    timer->callback_fn(timer->callback_ctx, now_ms);
}

void TimingWheel::cascade_wheel(uint32_t wheel_idx, uint32_t now_ms) {
    if (wheel_idx == 0 || wheel_idx >= kWheelCount) {
        return;
    }

    Wheel& wheel = m_wheels[wheel_idx];
    const uint32_t slot_idx = wheel.current_slot;
    Slot& slot = wheel.slots[slot_idx];

    Timer* current = slot.head;
    slot.head = nullptr;
    slot.tail = nullptr;

    while (current) {
        Timer* next = current->tw_next;

        current->tw_prev = nullptr;
        current->tw_next = nullptr;
        current->flags &= ~Timer::FLAG_SCHEDULED;

        const uint32_t delay_ms = compute_delay_ms(current->expires_at_ms, now_ms);

        uint8_t new_wheel = 0;
        uint8_t new_slot = 0;
        determine_wheel_and_slot(delay_ms, new_wheel, new_slot);

        const uint32_t base_slot = m_wheels[new_wheel].current_slot;
        const uint32_t slot_mask = kSlotsPerWheel - 1u;
        new_slot = (uint8_t)((base_slot + new_slot) & slot_mask);

        link_timer(current, new_wheel, new_slot);

        current = next;
    }

    const uint32_t slot_mask = kSlotsPerWheel - 1u;
    wheel.current_slot = (slot_idx + 1u) & slot_mask;

    if (wheel.current_slot == 0 && wheel_idx + 1u < kWheelCount) {
        cascade_wheel(wheel_idx + 1u, now_ms);
    }
}

void TimingWheel::process_slot(uint32_t wheel_idx, uint32_t slot_idx, uint32_t now_ms) {
    if (wheel_idx >= kWheelCount) {
        return;
    }

    Wheel& wheel = m_wheels[wheel_idx];
    Slot& slot = wheel.slots[slot_idx];

    Timer* current = slot.head;
    slot.head = nullptr;
    slot.tail = nullptr;

    while (current) {
        Timer* next = current->tw_next;

        current->tw_prev = nullptr;
        current->tw_next = nullptr;
        current->flags &= ~Timer::FLAG_SCHEDULED;

        if (!(current->flags & Timer::FLAG_CANCELLED)) {
            fire_timer(current, now_ms);

            if (m_active_count > 0) {
                m_active_count--;
            }
        }

        free_timer(current);

        current = next;
    }
}

void TimingWheel::advance_wheel(uint32_t wheel_idx, uint32_t now_ms) {
    if (wheel_idx >= kWheelCount) {
        return;
    }

    Wheel& wheel = m_wheels[wheel_idx];
    const uint32_t slot_idx = wheel.current_slot;

    process_slot(wheel_idx, slot_idx, now_ms);

    const uint32_t slot_mask = kSlotsPerWheel - 1u;
    wheel.current_slot = (slot_idx + 1u) & slot_mask;

    if (wheel.current_slot == 0 && wheel_idx + 1u < kWheelCount) {
        cascade_wheel(wheel_idx + 1u, now_ms);
    }
}

void TimingWheel::tick(uint32_t now_ms) {
    if (now_ms < m_current_time_ms) {
        return;
    }

    while (m_current_time_ms < now_ms) {
        advance_wheel(0, m_current_time_ms);
        m_current_time_ms++;
    }
}

bool TimingWheel::has_pending_timers() const {
    return m_active_count > 0;
}

}
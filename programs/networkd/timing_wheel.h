#ifndef YOS_NETD_TIMING_WHEEL_H
#define YOS_NETD_TIMING_WHEEL_H

#include "arena.h"

#include <stdint.h>

namespace netd {

class TimingWheel;

struct Timer {
    using CallbackFn = void (*)(void* ctx, uint32_t now_ms);

    Timer* tw_prev;
    Timer* tw_next;
    
    void* callback_ctx;
    CallbackFn callback_fn;
    
    uint32_t expires_at_ms;
    uint32_t generation;
    
    uint8_t wheel_index;
    uint8_t slot_index;
    uint16_t flags;
    
    static constexpr uint16_t FLAG_SCHEDULED = 0x0001u;
    static constexpr uint16_t FLAG_CANCELLED = 0x0002u;
};

struct TimerId {
    uint32_t index;
    uint32_t generation;
    
    constexpr TimerId() : index(0xFFFFFFFFu), generation(0) {
    }
    
    constexpr TimerId(uint32_t idx, uint32_t gen) : index(idx), generation(gen) {
    }
    
    constexpr bool is_valid() const {
        return index != 0xFFFFFFFFu;
    }
    
    constexpr bool operator==(const TimerId& other) const {
        return index == other.index && generation == other.generation;
    }
    
    constexpr bool operator!=(const TimerId& other) const {
        return !(*this == other);
    }
};

class TimingWheel {
public:
    static constexpr uint32_t kWheelCount = 4;
    static constexpr uint32_t kSlotsPerWheel = 256;
    static constexpr uint32_t kMaxTimers = 4096;
    
    static constexpr uint32_t kWheel0Granularity = 1u;
    static constexpr uint32_t kWheel1Granularity = 256u;
    static constexpr uint32_t kWheel2Granularity = 65536u;
    static constexpr uint32_t kWheel3Granularity = 16777216u;

    explicit TimingWheel(Arena& arena);
    
    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;
    
    bool init(uint32_t now_ms);
    
    TimerId schedule(uint32_t delay_ms, void* ctx, Timer::CallbackFn fn, uint32_t now_ms);
    
    TimerId schedule_at(uint32_t expires_at_ms, void* ctx, Timer::CallbackFn fn, uint32_t now_ms);
    
    bool cancel(TimerId timer_id);
    
    void tick(uint32_t now_ms);
    
    bool try_get_next_expiry_ms(uint32_t now_ms, uint32_t& out_ms) const;
    
    uint32_t timer_count() const {
        return m_active_count;
    }
    
    uint32_t capacity() const {
        return kMaxTimers;
    }

private:
    struct Slot {
        Timer* head;
        Timer* tail;
        
        Slot() : head(nullptr), tail(nullptr) {
        }
    };
    
    struct Wheel {
        Slot slots[kSlotsPerWheel];
        uint32_t current_slot;
        uint32_t granularity_ms;
        
        Wheel() : slots(), current_slot(0), granularity_ms(0) {
        }
    };
    
    Timer* alloc_timer();
    void free_timer(Timer* timer);
    
    void link_timer(Timer* timer, uint8_t wheel_idx, uint8_t slot_idx);
    void unlink_timer(Timer* timer);
    
    void fire_timer(Timer* timer, uint32_t now_ms);
    void cascade_wheel(uint32_t wheel_idx, uint32_t now_ms);
    
    uint32_t compute_delay_ms(uint32_t expires_at_ms, uint32_t now_ms) const;
    void determine_wheel_and_slot(uint32_t delay_ms, uint8_t& out_wheel, uint8_t& out_slot) const;
    
    void advance_wheel(uint32_t wheel_idx, uint32_t now_ms);
    void process_slot(uint32_t wheel_idx, uint32_t slot_idx, uint32_t now_ms);
    
    Arena& m_arena;
    
    Wheel m_wheels[kWheelCount];
    
    Timer* m_timer_pool;
    uint32_t m_pool_size;
    
    Timer* m_free_list;
    uint32_t m_free_count;
    
    uint32_t m_active_count;
    uint32_t m_current_time_ms;
    uint32_t m_next_generation;
};

}

#endif
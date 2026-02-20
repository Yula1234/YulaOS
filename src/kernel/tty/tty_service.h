#ifndef KERNEL_TTY_SERVICE_H
#define KERNEL_TTY_SERVICE_H

#include <stdint.h>

#include <lib/cpp/lock_guard.h>

#include <lib/cpp/atomic.h>
#include <lib/cpp/semaphore.h>

struct tty_handle;
using tty_handle_t = tty_handle;

namespace kernel::tty {

class TtySession;

class TtyService {
public:
    enum class InitState : uint32_t {
        Uninit = 0,
        Initing = 1,
        Ready = 2,
    };

    enum class RenderReason : uint32_t {
        Output = 1u << 0,
        Scroll = 1u << 1,
        Resize = 1u << 2,
        ActiveChanged = 1u << 3,
    };

    static TtyService& instance();

    TtyService(const TtyService&) = delete;
    TtyService& operator=(const TtyService&) = delete;

    TtyService(TtyService&&) = delete;
    TtyService& operator=(TtyService&&) = delete;

    tty_handle_t* get_active_for_render();
    void set_active(tty_handle_t* tty);
    void clear_active_if_matches(tty_handle_t* tty);

    void register_session(TtySession* session);
    void unregister_session(TtySession* session);

    void request_render(RenderReason reason);
    uint32_t consume_render_requests();

    void render_wakeup();
    void render_wait();
    int render_try_acquire();

private:
    TtyService();

    void ensure_init();

    static constexpr uint32_t encode_init_state(InitState v) {
        return static_cast<uint32_t>(v);
    }

    static constexpr InitState decode_init_state(uint32_t v) {
        return static_cast<InitState>(v);
    }

    kernel::atomic<uint32_t> m_init_state;

    kernel::SpinLock m_active_lock;
    tty_handle_t* m_active;

    kernel::SpinLock m_sessions_lock;
    TtySession* m_sessions_head;

    kernel::atomic<uint32_t> m_pending_render;
    kernel::atomic<uint32_t> m_render_reasons;

    kernel::Semaphore m_render_sem;
};

}

#endif

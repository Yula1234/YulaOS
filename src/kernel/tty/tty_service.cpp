#include <kernel/tty/tty_service.h>

#include <kernel/tty/tty_session.h>

#include <lib/cpp/new.h>

namespace kernel::tty {

TtyService& TtyService::instance() {
    static TtyService svc;
    return svc;
}

TtyService::TtyService()
    : m_init_state(encode_init_state(InitState::Uninit))
    , m_active(nullptr)
    , m_sessions_head(nullptr)
    , m_pending_render(0u)
    , m_render_reasons(0u)
    , m_render_sem(0) {
}

void TtyService::ensure_init() {
    InitState state = decode_init_state(m_init_state.load(kernel::memory_order::acquire));
    if (state == InitState::Ready) {
        return;
    }

    if (state == InitState::Uninit) {
        uint32_t expected = encode_init_state(InitState::Uninit);
        if (
            m_init_state.compare_exchange_strong(
                expected,
                encode_init_state(InitState::Initing),
                kernel::memory_order::acq_rel,
                kernel::memory_order::acquire
            )
        ) {
            m_init_state.store(
                encode_init_state(InitState::Ready),
                kernel::memory_order::release
            );
            return;
        }
    }

    while (
        decode_init_state(m_init_state.load(kernel::memory_order::acquire)) != InitState::Ready
    ) {
        kernel::cpu_relax();
    }
}

tty_handle_t* TtyService::get_active_for_render() {
    ensure_init();

    kernel::SpinLockSafeGuard g(m_active_lock);
    tty_handle_t* cur = m_active;

    return cur;
}

void TtyService::set_active(tty_handle_t* tty) {
    ensure_init();

    kernel::SpinLockSafeGuard g(m_active_lock);
    m_active = tty;
}

void TtyService::clear_active_if_matches(tty_handle_t* tty) {
    ensure_init();

    kernel::SpinLockSafeGuard g(m_active_lock);
    if (m_active == tty) {
        m_active = nullptr;
    }
}

void TtyService::register_session(TtySession* session) {
    if (!session) {
        return;
    }

    ensure_init();

    kernel::SpinLockSafeGuard g(m_sessions_lock);

    if (!m_sessions_head) {
        m_sessions_head = session;
    } else {
        session->link_before(m_sessions_head);
        m_sessions_head = session;
    }
}

void TtyService::request_render(RenderReason reason) {
    ensure_init();

    m_render_reasons.fetch_or(static_cast<uint32_t>(reason), kernel::memory_order::acq_rel);

    if (m_pending_render.exchange(1u, kernel::memory_order::acq_rel) == 0u) {
        m_render_sem.signal();
    }
}

uint32_t TtyService::consume_render_requests() {
    ensure_init();

    m_pending_render.store(0u, kernel::memory_order::release);

    return m_render_reasons.exchange(0u, kernel::memory_order::acq_rel);
}

void TtyService::unregister_session(TtySession* session) {
    if (!session) {
        return;
    }

    ensure_init();

    kernel::SpinLockSafeGuard g(m_sessions_lock);

    if (m_sessions_head == session) {
        m_sessions_head = session->next();
    }

    session->unlink();
}

void TtyService::render_wakeup() {
    ensure_init();
    m_render_sem.signal();
}

void TtyService::render_wait() {
    ensure_init();
    m_render_sem.wait();
}

int TtyService::render_try_acquire() {
    ensure_init();
    return m_render_sem.try_acquire() ? 1 : 0;
}

}

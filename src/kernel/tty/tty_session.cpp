#include <kernel/tty/tty_session.h>

#include <kernel/tty/tty_service.h>

#include <lib/cpp/new.h>

#include <mm/heap.h>

namespace kernel::tty {

TtySession* TtySession::create(int cols, int view_rows) {
    kernel::term::Term* term = new (kernel::nothrow) kernel::term::Term(cols, view_rows);
    if (!term) {
        return nullptr;
    }

    TtySession* session = new (kernel::nothrow) TtySession(term);
    if (!session) {
        delete term;
        return nullptr;
    }

    TtyService::instance().register_session(session);

    return session;
}

TtySession::TtySession(kernel::term::Term* term)
    : m_term(term)
    , m_prev(nullptr)
    , m_next(nullptr) {
}

TtySession::~TtySession() {
    TtyService::instance().unregister_session(this);

    if (m_term) {
        delete m_term;
        m_term = nullptr;
    }
}

kernel::term::Term* TtySession::term() {
    return m_term;
}

const kernel::term::Term* TtySession::term() const {
    return m_term;
}

TtySession* TtySession::prev() const {
    return m_prev;
}

TtySession* TtySession::next() const {
    return m_next;
}

void TtySession::link_before(TtySession* node) {
    if (!node) {
        return;
    }

    m_next = node;
    m_prev = node->m_prev;

    node->m_prev = this;

    if (m_prev) {
        m_prev->m_next = this;
    }
}

void TtySession::unlink() {
    if (m_prev) {
        m_prev->m_next = m_next;
    }

    if (m_next) {
        m_next->m_prev = m_prev;
    }

    m_prev = nullptr;
    m_next = nullptr;
}

}

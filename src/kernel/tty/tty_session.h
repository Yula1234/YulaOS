#ifndef KERNEL_TTY_SESSION_H
#define KERNEL_TTY_SESSION_H

#include <stdint.h>

#include <lib/dlist.h>

#include <kernel/term/term.h>

namespace kernel::tty {

class TtySession {
public:
    static TtySession* create(int cols, int view_rows);

    TtySession(const TtySession&) = delete;
    TtySession& operator=(const TtySession&) = delete;

    TtySession(TtySession&&) = delete;
    TtySession& operator=(TtySession&&) = delete;

    ~TtySession();

    kernel::term::Term* term();
    const kernel::term::Term* term() const;

private:
    explicit TtySession(kernel::term::Term* term);

    friend class TtyService;

    dlist_head_t m_sessions_node{};

    kernel::term::Term* m_term;
};

}

#endif

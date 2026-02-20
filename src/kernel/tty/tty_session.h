#ifndef KERNEL_TTY_SESSION_H
#define KERNEL_TTY_SESSION_H

#include <stdint.h>

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

    TtySession* prev() const;
    TtySession* next() const;

private:
    explicit TtySession(kernel::term::Term* term);

    friend class TtyService;

    void link_before(TtySession* node);
    void unlink();

    kernel::term::Term* m_term;

    TtySession* m_prev;
    TtySession* m_next;
};

}

#endif

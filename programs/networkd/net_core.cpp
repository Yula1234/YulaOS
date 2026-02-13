#include "net_core.h"

#include <yula.h>

namespace netd {

void UniqueFd::reset(int v) {
    if (fd >= 0) {
        close(fd);
    }

    fd = v;
}

PipePair::PipePair() : m_r(), m_w() {
}

bool PipePair::create() {
    int fds[2] = { -1, -1 };
    if (pipe(fds) != 0) {
        return false;
    }

    m_r.reset(fds[0]);
    m_w.reset(fds[1]);
    return true;
}

void PipePair::signal() const {
    if (m_w.get() < 0) {
        return;
    }

    const uint8_t b = 1u;
    (void)pipe_try_write(m_w.get(), &b, 1u);
}

void PipePair::drain() const {
    if (m_r.get() < 0) {
        return;
    }

    uint8_t buf[64];
    for (;;) {
        const int got = pipe_try_read(m_r.get(), buf, (uint32_t)sizeof(buf));
        if (got <= 0) {
            break;
        }
    }
}

}

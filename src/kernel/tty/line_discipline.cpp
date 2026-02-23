#include <kernel/tty/line_discipline.h>

#include <stddef.h>
#include <stdint.h>

namespace kernel::tty {

static bool is_backspace(uint8_t b) {
    return b == 0x08u || b == 0x7Fu;
}

static bool is_newline(uint8_t b) {
    return b == '\n';
}

LineDiscipline::LineDiscipline()
    : sem_(0)
    , cfg_()
    , cooked_()
    , line_{}
    , line_len_(0)
    , echo_emit_(nullptr)
    , echo_emit_ctx_(nullptr)
    , signal_emit_(nullptr)
    , signal_emit_ctx_(nullptr) {
}

void LineDiscipline::set_config(LineDisciplineConfig cfg) {
    kernel::SpinLockSafeGuard g(lock_);
    cfg_ = cfg;
}

LineDisciplineConfig LineDiscipline::config() const {
    kernel::SpinLockSafeGuard g(lock_);
    return cfg_;
}

void LineDiscipline::set_echo_emitter(
    emit_fn emit,
    void* ctx
) {
    kernel::SpinLockSafeGuard g(lock_);

    echo_emit_ = emit;
    echo_emit_ctx_ = ctx;
}

void LineDiscipline::set_signal_emitter(
    signal_fn emit,
    void* ctx
) {
    kernel::SpinLockSafeGuard g(lock_);

    signal_emit_ = emit;
    signal_emit_ctx_ = ctx;
}

bool LineDiscipline::Ring::push(uint8_t b) {
    if (count == cooked_cap) {
        return false;
    }

    data[head] = b;
    head++;
    if (head == cooked_cap) {
        head = 0;
    }

    count++;
    return true;
}

bool LineDiscipline::Ring::pop(uint8_t& out) {
    if (count == 0) {
        return false;
    }

    out = data[tail];
    tail++;
    if (tail == cooked_cap) {
        tail = 0;
    }

    count--;
    return true;
}

bool LineDiscipline::Ring::pop_if(uint8_t& out, bool (*pred)(uint8_t b, void* ctx), void* ctx) {
    if (count == 0) {
        return false;
    }

    uint8_t b = data[tail];
    if (!pred(b, ctx)) {
        return false;
    }

    return pop(out);
}

bool LineDiscipline::pred_always(uint8_t, void*) {
    return true;
}

bool LineDiscipline::pred_until_newline(uint8_t b, void* ctx) {
    bool* out_done = static_cast<bool*>(ctx);
    if (*out_done) {
        return false;
    }

    if (b == '\n') {
        *out_done = true;
    }

    return true;
}

void LineDiscipline::cooked_push_locked(uint8_t b) {
    if (!cooked_.push(b)) {
        uint8_t drop = 0;
        (void)cooked_.pop(drop);
        (void)cooked_.push(b);
    }
}

void LineDiscipline::cooked_signal_locked() {
    sem_.signal();
}

void LineDiscipline::echo_byte_locked(uint8_t b) {
    if (!cfg_.echo) {
        return;
    }

    if (!echo_emit_) {
        return;
    }

    (void)echo_emit_(&b, 1, echo_emit_ctx_);
}

void LineDiscipline::echo_erase_locked() {
    if (!cfg_.echo) {
        return;
    }

    if (!echo_emit_) {
        return;
    }

    const uint8_t seq[3] = { '\b', ' ', '\b' };
    (void)echo_emit_(seq, sizeof(seq), echo_emit_ctx_);
}

void LineDiscipline::echo_signal_locked(int sig) {
    if (!cfg_.echo) {
        return;
    }

    if (!echo_emit_) {
        return;
    }

    uint8_t seq[4];
    size_t n = 0;

    seq[n++] = '^';

    if (sig == 2) {
        seq[n++] = 'C';
    } else if (sig == 3) {
        seq[n++] = '\\';
    } else if (sig == 20) {
        seq[n++] = 'Z';
    } else {
        seq[n++] = '?';
    }

    seq[n++] = '\n';
    (void)echo_emit_(seq, n, echo_emit_ctx_);
}

bool LineDiscipline::try_isig_locked(uint8_t b) {
    if (!cfg_.isig) {
        return false;
    }

    if (!signal_emit_) {
        return false;
    }

    int sig = 0;
    if (b == cfg_.vintr) {
        sig = 2;
    } else if (b == cfg_.vquit) {
        sig = 3;
    } else if (b == cfg_.vsusp) {
        sig = 20;
    }

    if (sig == 0) {
        return false;
    }

    echo_signal_locked(sig);
    signal_emit_(sig, signal_emit_ctx_);
    return true;
}

void LineDiscipline::receive_byte_locked(uint8_t b) {
    if (try_isig_locked(b)) {
        return;
    }

    if (!cfg_.canonical) {
        cooked_push_locked(b);
        echo_byte_locked(b);
        cooked_signal_locked();
        return;
    }

    if (is_backspace(b)) {
        if (line_len_ > 0) {
            line_len_--;
            echo_erase_locked();
        }
        return;
    }

    if (b == '\r') {
        b = '\n';
    }

    if (line_len_ < line_cap) {
        line_[line_len_++] = b;
    }

    echo_byte_locked(b);

    if (is_newline(b)) {
        for (size_t i = 0; i < line_len_; i++) {
            cooked_push_locked(line_[i]);
        }

        line_len_ = 0;
        cooked_signal_locked();
    }
}

void LineDiscipline::receive_bytes(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return;
    }

    kernel::SpinLockSafeGuard g(lock_);

    for (size_t i = 0; i < size; i++) {
        receive_byte_locked(data[i]);
    }
}

bool LineDiscipline::has_readable_locked() const {
    if (!cfg_.canonical) {
        return cooked_.count != 0;
    }

    for (size_t i = 0; i < cooked_.count; i++) {
        size_t idx = cooked_.tail + i;
        if (idx >= cooked_cap) {
            idx -= cooked_cap;
        }

        if (cooked_.data[idx] == '\n') {
            return true;
        }
    }

    return false;
}

bool LineDiscipline::has_readable() const {
    kernel::SpinLockSafeGuard g(lock_);
    return has_readable_locked();
}

size_t LineDiscipline::read(void* out, size_t size) {
    if (!out || size == 0) {
        return 0;
    }

    for (;;) {
        {
            kernel::SpinLockSafeGuard g(lock_);

            if (has_readable_locked()) {
                break;
            }
        }

        sem_.wait();
    }

    uint8_t* dst = static_cast<uint8_t*>(out);

    kernel::SpinLockSafeGuard g(lock_);

    size_t n = 0;
    if (!cfg_.canonical) {
        while (n < size) {
            uint8_t b = 0;
            if (!cooked_.pop(b)) {
                break;
            }

            dst[n++] = b;
        }

        return n;
    }

    bool done = false;
    while (n < size) {
        uint8_t b = 0;
        if (!cooked_.pop_if(b, pred_until_newline, &done)) {
            break;
        }

        dst[n++] = b;
        if (done) {
            break;
        }
    }

    return n;
}

size_t LineDiscipline::write_transform(
    const void* in,
    size_t size,
    emit_fn emit,
    void* ctx
) {
    if (!in || size == 0) {
        return 0;
    }

    if (!emit) {
        return 0;
    }

    const uint8_t* src = static_cast<const uint8_t*>(in);

    kernel::SpinLockSafeGuard g(lock_);

    size_t produced = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = src[i];

        if (cfg_.onlcr && b == '\n') {
            const uint8_t seq[2] = { '\r', '\n' };
            size_t w = emit(seq, sizeof(seq), ctx);
            produced += w;
            if (w != sizeof(seq)) {
                break;
            }
            continue;
        }

        size_t w = emit(&b, 1, ctx);
        produced += w;
        if (w != 1) {
            break;
        }
    }

    return produced;
}

}

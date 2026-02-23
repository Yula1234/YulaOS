#pragma once

#include <lib/cpp/semaphore.h>
#include <lib/cpp/lock_guard.h>

#include <stddef.h>
#include <stdint.h>

namespace kernel::tty {

struct LineDisciplineConfig {
    bool canonical = true;
    bool echo = true;
    bool onlcr = true;

    bool isig = false;
    uint8_t vintr = 0x03u;
    uint8_t vquit = 0x1Cu;
    uint8_t vsusp = 0x1Au;
};

class LineDiscipline {
public:
    using emit_fn = size_t (*)(const uint8_t* data, size_t size, void* ctx);
    using signal_fn = void (*)(int sig, void* ctx);

    LineDiscipline();

    LineDiscipline(const LineDiscipline&) = delete;
    LineDiscipline& operator=(const LineDiscipline&) = delete;

    LineDiscipline(LineDiscipline&&) = delete;
    LineDiscipline& operator=(LineDiscipline&&) = delete;

    void set_config(LineDisciplineConfig cfg);
    LineDisciplineConfig config() const;

    void set_echo_emitter(
        emit_fn emit,
        void* ctx
    );

    void set_signal_emitter(
        signal_fn emit,
        void* ctx
    );

    void receive_bytes(const uint8_t* data, size_t size);

    size_t read(void* out, size_t size);
    size_t write_transform(
        const void* in,
        size_t size,
        emit_fn emit,
        void* ctx
    );

    bool has_readable() const;

private:
    static constexpr size_t cooked_cap = 4096;
    static constexpr size_t line_cap = 512;

    struct Ring {
        uint8_t data[cooked_cap];
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;

        bool push(uint8_t b);
        bool pop(uint8_t& out);
        bool pop_if(uint8_t& out, bool (*pred)(uint8_t b, void* ctx), void* ctx);
    };

    void receive_byte_locked(uint8_t b);

    void cooked_push_locked(uint8_t b);
    void cooked_signal_locked();

    void echo_byte_locked(uint8_t b);
    void echo_erase_locked();

    void echo_signal_locked(int sig);
    bool try_isig_locked(uint8_t b);

    static bool pred_always(uint8_t, void*);
    static bool pred_until_newline(uint8_t b, void*);

    bool has_readable_locked() const;

    mutable kernel::SpinLock lock_;
    kernel::Semaphore sem_;

    LineDisciplineConfig cfg_;

    Ring cooked_;

    uint8_t line_[line_cap];
    size_t line_len_;

    emit_fn echo_emit_ = nullptr;
    void* echo_emit_ctx_ = nullptr;

    signal_fn signal_emit_ = nullptr;
    void* signal_emit_ctx_ = nullptr;
};

}

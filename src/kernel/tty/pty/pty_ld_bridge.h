#pragma once

#include <stddef.h>
#include <stdint.h>

#include <yos/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef size_t (*pty_ld_emit_fn)(const uint8_t* data, size_t size, void* ctx);

typedef void (*pty_ld_signal_fn)(int sig, void* ctx);

typedef struct pty_ld_handle pty_ld_handle_t;

pty_ld_handle_t* pty_ld_create(
    const yos_termios_t* termios,
    pty_ld_emit_fn echo_emit,
    void* echo_ctx,
    pty_ld_signal_fn sig_emit,
    void* sig_ctx
);

void pty_ld_destroy(pty_ld_handle_t* h);

int pty_ld_set_termios(pty_ld_handle_t* h, const yos_termios_t* termios);

void pty_ld_receive(pty_ld_handle_t* h, const uint8_t* data, size_t size);

size_t pty_ld_read(pty_ld_handle_t* h, void* out, size_t size);

size_t pty_ld_write(pty_ld_handle_t* h, const void* in, size_t size);

int pty_ld_has_readable(pty_ld_handle_t* h);

#ifdef __cplusplus
}
#endif

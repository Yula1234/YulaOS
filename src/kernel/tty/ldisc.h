/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ldisc_config {
    bool canonical_;
    bool echo_;
    bool onlcr_;

    bool igncr_;
    bool icrnl_;
    bool inlcr_;

    bool opost_;

    uint8_t vmin_;
    uint8_t vtime_;

    bool isig_;
    uint8_t vintr_;
    uint8_t vquit_;
    uint8_t vsusp_;
    uint8_t veof_;
} ldisc_config_t;

typedef struct ldisc ldisc_t;

typedef size_t (*ldisc_emit_fn_t)(const uint8_t* data, size_t size, void* ctx);
typedef void (*ldisc_signal_fn_t)(int sig, void* ctx);

ldisc_t* ldisc_create(void);

void ldisc_destroy(ldisc_t* ld);

void ldisc_set_config(ldisc_t* ld, const ldisc_config_t* cfg);

void ldisc_get_config(const ldisc_t* ld, ldisc_config_t* out_cfg);

void ldisc_set_callbacks(
    ldisc_t* ld, ldisc_emit_fn_t echo_emit,
    void* echo_ctx, ldisc_signal_fn_t sig_emit,
    void* sig_ctx
);

void ldisc_receive(ldisc_t* ld, const uint8_t* data, size_t size);

size_t ldisc_read(ldisc_t* ld, void* out, size_t size);

size_t ldisc_write_transform(
    ldisc_t* ld, const void* in,
    size_t size, ldisc_emit_fn_t emit,
    void* ctx
);

bool ldisc_has_readable(const ldisc_t* ld);

#ifdef __cplusplus
}
#endif
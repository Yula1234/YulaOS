// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_TLS_H
#define YOS_NETWORKD_TLS_H

#include <stdint.h>

#include "netd_types.h"

#define NETD_TLS_RX_CAP 8192u

typedef struct {
    netd_tcp_conn_t* tcp;
    int active;
    int ready;
    int closed;

    uint16_t suite;
    uint8_t key_len;
    uint8_t pad0;

    uint8_t hs_key_r[32];
    uint8_t hs_iv_r[12];
    uint64_t hs_seq_r;

    uint8_t hs_key_w[32];
    uint8_t hs_iv_w[12];
    uint64_t hs_seq_w;

    uint8_t app_key_r[32];
    uint8_t app_iv_r[12];
    uint64_t app_seq_r;

    uint8_t app_key_w[32];
    uint8_t app_iv_w[12];
    uint64_t app_seq_w;

    int prot_read;
    int prot_write;

    uint32_t hs_step;
    uint32_t hs_status;
    uint16_t hs_alert;
    uint16_t pad1;

    uint8_t rx_buf[NETD_TLS_RX_CAP];
    uint32_t rx_r;
    uint32_t rx_w;
} netd_tls_client_t;

int netd_tls_handshake(netd_ctx_t* ctx, netd_tls_client_t* t, netd_tcp_conn_t* tcp, const char* host, uint32_t timeout_ms);

int netd_tls_connect(netd_ctx_t* ctx, netd_tls_client_t* t, const char* host, uint32_t ip, uint16_t port, uint32_t timeout_ms);

int netd_tls_send(netd_ctx_t* ctx, netd_tls_client_t* t, const void* data, uint32_t len, uint32_t timeout_ms);

int netd_tls_recv(netd_ctx_t* ctx, netd_tls_client_t* t, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n);

int netd_tls_close(netd_ctx_t* ctx, netd_tls_client_t* t, uint32_t timeout_ms);

#endif

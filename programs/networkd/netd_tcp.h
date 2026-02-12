// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_TCP_H
#define YOS_NETWORKD_TCP_H

#include <stdint.h>

#include "netd_proto.h"
#include "netd_types.h"

void netd_tcp_init(netd_ctx_t* ctx);
void netd_tcp_shutdown(netd_ctx_t* ctx);

void netd_tcp_process_ipv4(netd_ctx_t* ctx, const net_ipv4_hdr_t* ip, const uint8_t* payload, uint32_t payload_len);

netd_tcp_conn_t* netd_tcp_open(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms, uint32_t* out_status);

netd_tcp_conn_t* netd_tcp_open_start(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint32_t* out_status);

int netd_tcp_open_poll(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t start_ms, uint32_t timeout_ms, uint32_t* out_status);

uint32_t netd_tcp_recv_nowait(netd_tcp_conn_t* c, void* out, uint32_t cap);

int netd_tcp_send_poll(netd_ctx_t* ctx, netd_tcp_conn_t* c, const void* data, uint32_t len, uint32_t* io_off, uint32_t start_ms, uint32_t timeout_ms, uint32_t* out_status);

int netd_tcp_close_start(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t* out_status);

int netd_tcp_close_poll(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t start_ms, uint32_t timeout_ms, uint32_t* out_status);

int netd_tcp_send(netd_ctx_t* ctx, netd_tcp_conn_t* c, const void* data, uint32_t len, uint32_t timeout_ms);

int netd_tcp_recv(netd_ctx_t* ctx, netd_tcp_conn_t* c, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n);

int netd_tcp_close(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t timeout_ms);

#endif

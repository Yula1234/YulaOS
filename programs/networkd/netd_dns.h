// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_DNS_H
#define YOS_NETWORKD_DNS_H

#include <stdint.h>

#include "netd_types.h"
#include "netd_proto.h"

void netd_dns_process_udp(netd_ctx_t* ctx, const net_ipv4_hdr_t* ip, const uint8_t* payload, uint32_t payload_len);

int netd_dns_query(netd_ctx_t* ctx, const char* name, uint32_t timeout_ms, uint32_t* out_addr);

int netd_dns_query_start(netd_ctx_t* ctx, const char* name, uint32_t timeout_ms);

int netd_dns_query_poll(netd_ctx_t* ctx, int handle, uint32_t* out_addr, uint32_t* out_status);

void netd_dns_query_cancel(netd_ctx_t* ctx, int handle);

#endif


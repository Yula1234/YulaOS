// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_IPV4_H
#define YOS_NETWORKD_IPV4_H

#include <stdint.h>

#include "netd_types.h"

void netd_ipv4_process_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len);

uint32_t netd_ipv4_send_ping(netd_ctx_t* ctx, uint32_t dst_ip, uint32_t timeout_ms, uint16_t seq, uint32_t* out_rtt);

#endif


// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_UDP_H
#define YOS_NETWORKD_UDP_H

#include <stdint.h>

#include "netd_types.h"

int netd_udp_send(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t* payload, uint32_t payload_len);

#endif


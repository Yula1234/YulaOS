// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_ARP_H
#define YOS_NETWORKD_ARP_H

#include <stdint.h>

#include "netd_types.h"

void netd_arp_init(netd_ctx_t* ctx);

void netd_arp_process_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len);

int netd_arp_resolve_mac(netd_ctx_t* ctx, uint32_t target_ip, uint8_t out_mac[6], uint32_t timeout_ms);

#endif


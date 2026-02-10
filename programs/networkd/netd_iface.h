// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_IFACE_H
#define YOS_NETWORKD_IFACE_H

#include <stdint.h>

#include "netd_types.h"

int netd_iface_init(netd_ctx_t* ctx);
void netd_iface_close(netd_ctx_t* ctx);
int netd_iface_ensure_up(netd_ctx_t* ctx);
void netd_iface_periodic(netd_ctx_t* ctx);

void netd_iface_print_state(const netd_ctx_t* ctx);

void netd_links_init(netd_ctx_t* ctx);

uint32_t netd_iface_next_hop_ip(const netd_ctx_t* ctx, uint32_t dst_ip);

int netd_iface_read_frame(netd_ctx_t* ctx, uint8_t* buf, uint32_t cap);
int netd_iface_send_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len);

#endif

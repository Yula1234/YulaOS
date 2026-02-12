// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_HTTP_H
#define YOS_NETWORKD_HTTP_H

#include <stdint.h>

#include <net_ipc.h>

#include "netd_types.h"

int netd_http_get(netd_ctx_t* ctx, int fd_out, uint32_t seq, const net_http_get_req_t* req);

int netd_http_get_start(netd_ctx_t* ctx, int fd_out, uint32_t seq, const net_http_get_req_t* req);

void netd_http_tick(netd_ctx_t* ctx);

#endif

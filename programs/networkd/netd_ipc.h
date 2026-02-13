// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_IPC_H
#define YOS_NETWORKD_IPC_H

#include <stdint.h>

#include "netd_types.h"

void netd_ipc_clients_init(netd_client_t* clients, uint32_t capacity);

void netd_ipc_accept_pending(netd_ctx_t* ctx, int listen_fd);

void netd_ipc_process_clients(netd_ctx_t* ctx, netd_client_t* clients, uint32_t count);

#endif


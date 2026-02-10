// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKCTL_IPC_H
#define YOS_NETWORKCTL_IPC_H

#include "netctl_common.h"

typedef struct {
    int fd_r;
    int fd_w;
    net_ipc_rx_t rx;
    uint32_t seq;
} netctl_session_t;

int netctl_session_open(netctl_session_t* s);
void netctl_session_close(netctl_session_t* s);
int netctl_session_send_hello(netctl_session_t* s);

int netctl_wait(
    int fd,
    net_ipc_rx_t* rx,
    uint16_t want_type,
    uint32_t want_seq,
    net_ipc_hdr_t* out_hdr,
    uint8_t* out_payload,
    uint32_t cap,
    uint32_t timeout_ms
);

int netctl_dns_query(netctl_session_t* s, const char* name, uint32_t timeout_ms, uint32_t* out_addr);

#endif

// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_TYPES_H
#define YOS_NETWORKD_TYPES_H

#include <stdint.h>

#include <net_ipc.h>

#include "netd_config.h"
#include "netd_rand.h"

typedef struct {
    int used;
    int fd_in;
    int fd_out;
    net_ipc_rx_t rx;
} netd_client_t;

typedef struct {
    net_link_info_t links[4];
    uint32_t count;
} netd_state_t;

typedef struct {
    int fd;
    uint8_t mac[6];
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    int up;
} netd_iface_t;

typedef struct {
    int used;
    uint32_t ip;
    uint8_t mac[6];
} netd_arp_entry_t;

typedef struct {
    int active;
    int received;
    uint16_t id;
    uint16_t seq;
    uint32_t target_ip;
} netd_ping_wait_t;

typedef struct {
    int active;
    int received;
    uint16_t id;
    uint16_t port;
    uint32_t addr;
} netd_dns_wait_t;

#define NETD_TCP_RX_CAP 4096u

typedef struct {
    int active;
    uint8_t state;
    uint8_t pad0[3];

    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;

    uint32_t iss;
    uint32_t irs;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;

    int remote_closed;
    int fin_sent;
    int fin_acked;

    uint32_t last_activity_ms;
    uint32_t last_err;

    uint8_t rx_buf[NETD_TCP_RX_CAP];
    uint32_t rx_r;
    uint32_t rx_w;
} netd_tcp_conn_t;

typedef struct {
    netd_state_t state;
    netd_iface_t iface;
    uint32_t iface_last_try_ms;
    uint32_t dns_server;

    netd_arp_entry_t arp_cache[NETD_ARP_CACHE_SIZE];
    uint32_t arp_next_slot;

    netd_ping_wait_t ping_wait;
    netd_dns_wait_t dns_wait;
    netd_tcp_conn_t tcp;
    netd_rand_t rand;

    uint8_t rx_buf[NETD_FRAME_MAX];
    uint8_t tx_buf[NETD_FRAME_MAX];
} netd_ctx_t;

#endif


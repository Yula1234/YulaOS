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
    uint32_t req_count;
    uint32_t last_activity_ms;
} netd_client_t;

typedef struct {
    netd_client_t* clients;
    uint32_t count;
    uint32_t capacity;
} netd_ipc_ctx_t;

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
    uint32_t mtu;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
} netd_iface_t;

typedef struct {
    int used;
    uint32_t ip;
    uint8_t mac[6];
    uint32_t timestamp_ms;
    uint32_t ttl_ms;
} netd_arp_entry_t;

typedef struct {
    netd_arp_entry_t* entries;
    uint32_t count;
    uint32_t capacity;
    uint32_t next_slot;
    uint32_t hits;
    uint32_t misses;
    uint32_t timeouts;
} netd_arp_cache_t;

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

typedef struct {
    int active;
    int received;
    uint16_t id;
    uint16_t port;
    uint32_t addr;
    uint32_t start_ms;
    uint32_t timeout_ms;
} netd_dns_wait_slot_t;

typedef struct {
    char name[256];
    uint32_t addr;
    uint32_t timestamp_ms;
    uint32_t ttl_ms;
} netd_dns_cache_entry_t;

typedef struct {
    netd_dns_cache_entry_t* entries;
    uint32_t count;
    uint32_t capacity;
    uint32_t hits;
    uint32_t misses;
} netd_dns_cache_t;

typedef struct {
    netd_dns_wait_slot_t* slots;
    uint32_t count;
    uint32_t capacity;
} netd_dns_wait_mgr_t;

typedef struct {
    uint32_t rtt_ms;
    uint32_t rttvar_ms;
    uint32_t srtt_ms;
    uint32_t rto_ms;
} netd_tcp_rtt_t;

typedef struct {
    uint32_t ssthresh;
    uint32_t cwnd;
    uint32_t cwnd_bytes;
    uint32_t in_flight;
    uint8_t state;
    uint8_t dup_acks;
    uint16_t reserved;
} netd_tcp_congestion_t;

typedef struct {
    int active;
    uint8_t state;
    uint8_t flags;
    uint16_t pad0;

    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t mgr_index;

    uint32_t iss;
    uint32_t irs;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t snd_wnd;
    uint32_t rcv_wnd;

    int remote_closed;
    int fin_sent;
    int fin_acked;

    uint32_t last_activity_ms;
    uint32_t last_send_ms;
    uint32_t last_recv_ms;
    uint32_t last_err;

    uint8_t* rx_buf;
    uint32_t rx_cap;
    uint32_t rx_r;
    uint32_t rx_w;

    uint8_t* tx_buf;
    uint32_t tx_cap;
    uint32_t tx_r;
    uint32_t tx_w;

    netd_tcp_rtt_t rtt;
    netd_tcp_congestion_t cc;

    uint32_t mss;
    uint8_t window_scale;
    uint8_t timestamps_enabled;
    uint16_t pad1;

    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t retransmits;
    uint32_t fast_retransmits;
} netd_tcp_conn_t;

typedef struct {
    netd_tcp_conn_t** conns;
    uint32_t count;
    uint32_t cap;
    uint32_t* map;
    uint32_t map_cap;
    uint32_t total_connections;
    uint32_t active_connections;
    uint32_t failed_connections;
    uint64_t total_rx_bytes;
    uint64_t total_tx_bytes;
} netd_tcp_mgr_t;

typedef struct {
    uint64_t total_packets;
    uint64_t total_bytes;
    uint64_t icmp_packets;
    uint64_t udp_packets;
    uint64_t tcp_packets;
    uint64_t other_packets;
    uint64_t errors;
    uint64_t checksum_errors;
    uint64_t dropped;
} netd_ipv4_stats_t;

typedef struct {
    uint32_t requests;
    uint32_t replies;
    uint32_t timeouts;
    uint32_t cache_hits;
    uint32_t cache_misses;
} netd_arp_stats_t;

typedef struct {
    uint32_t queries;
    uint32_t responses;
    uint32_t timeouts;
    uint32_t cache_hits;
    uint32_t cache_misses;
} netd_dns_stats_t;

typedef struct {
    uint32_t connections;
    uint32_t active;
    uint32_t failed;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t retransmits;
    uint32_t timeouts;
} netd_tcp_stats_t;

typedef struct {
    uint32_t requests;
    uint32_t completed;
    uint32_t failed;
    uint32_t timeouts;
    uint32_t redirects;
} netd_http_stats_t;

typedef struct {
    netd_ipv4_stats_t ipv4;
    netd_arp_stats_t arp;
    netd_dns_stats_t dns;
    netd_tcp_stats_t tcp;
    netd_http_stats_t http;
    uint32_t start_time_ms;
} netd_stats_t;

typedef struct {
    netd_state_t state;
    netd_iface_t iface;
    uint32_t iface_last_try_ms;
    uint32_t dns_server;

    netd_arp_cache_t arp_cache;

    netd_ping_wait_t ping_wait;
    netd_dns_wait_t dns_wait;
    netd_dns_wait_mgr_t dns_waits;
    netd_dns_cache_t dns_cache;

    netd_tcp_mgr_t tcp;
    netd_rand_t rand;
    netd_ipc_ctx_t ipc;

    netd_stats_t stats;

    uint8_t* rx_buf;
    uint32_t rx_buf_size;
    uint8_t* tx_buf;
    uint32_t tx_buf_size;

    int log_level;
    int enable_stats;
} netd_ctx_t;

#endif
// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_STATS_H
#define YOS_NETWORKD_STATS_H

#include "netd_types.h"

void netd_stats_init(netd_stats_t* stats);

void netd_stats_reset(netd_stats_t* stats);

void netd_stats_print(const netd_stats_t* stats);

void netd_stats_ipv4_rx_packet(netd_stats_t* stats, uint32_t bytes, uint8_t proto);

void netd_stats_ipv4_tx_packet(netd_stats_t* stats, uint32_t bytes, uint8_t proto);

void netd_stats_ipv4_error(netd_stats_t* stats);

void netd_stats_ipv4_checksum_error(netd_stats_t* stats);

void netd_stats_ipv4_dropped(netd_stats_t* stats);

void netd_stats_arp_request(netd_stats_t* stats);

void netd_stats_arp_reply(netd_stats_t* stats);

void netd_stats_arp_timeout(netd_stats_t* stats);

void netd_stats_arp_cache_hit(netd_stats_t* stats);

void netd_stats_arp_cache_miss(netd_stats_t* stats);

void netd_stats_dns_query(netd_stats_t* stats);

void netd_stats_dns_response(netd_stats_t* stats);

void netd_stats_dns_timeout(netd_stats_t* stats);

void netd_stats_dns_cache_hit(netd_stats_t* stats);

void netd_stats_dns_cache_miss(netd_stats_t* stats);

void netd_stats_tcp_connection(netd_stats_t* stats);

void netd_stats_tcp_close(netd_stats_t* stats);

void netd_stats_tcp_failed(netd_stats_t* stats);

void netd_stats_tcp_rx_bytes(netd_stats_t* stats, uint64_t bytes);

void netd_stats_tcp_tx_bytes(netd_stats_t* stats, uint64_t bytes);

void netd_stats_tcp_retransmit(netd_stats_t* stats);

void netd_stats_tcp_timeout(netd_stats_t* stats);

void netd_stats_http_request(netd_stats_t* stats);

void netd_stats_http_completed(netd_stats_t* stats);

void netd_stats_http_failed(netd_stats_t* stats);

void netd_stats_http_timeout(netd_stats_t* stats);

void netd_stats_http_redirect(netd_stats_t* stats);

void netd_log(netd_ctx_t* ctx, int level, const char* fmt, ...);

#define netd_log_error(ctx, ...) netd_log(ctx, NETD_LOG_LEVEL_ERROR, __VA_ARGS__)
#define netd_log_warn(ctx, ...) netd_log(ctx, NETD_LOG_LEVEL_WARN, __VA_ARGS__)
#define netd_log_info(ctx, ...) netd_log(ctx, NETD_LOG_LEVEL_INFO, __VA_ARGS__)
#define netd_log_debug(ctx, ...) netd_log(ctx, NETD_LOG_LEVEL_DEBUG, __VA_ARGS__)

void netd_iface_stats_update_rx(netd_iface_t* iface, uint32_t bytes, int error);

void netd_iface_stats_update_tx(netd_iface_t* iface, uint32_t bytes, int error);

#endif
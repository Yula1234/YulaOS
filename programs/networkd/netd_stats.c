// SPDX-License-Identifier: GPL-2.0

#include "netd_stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <yula.h>

#include "netd_config.h"

void netd_stats_init(netd_stats_t* stats) {
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->start_time_ms = uptime_ms();
}

void netd_stats_reset(netd_stats_t* stats) {
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->start_time_ms = uptime_ms();
}

void netd_stats_print(const netd_stats_t* stats) {
    if (!stats) {
        return;
    }

    uint32_t uptime_sec = (uptime_ms() - stats->start_time_ms) / 1000u;
    uint32_t hours = uptime_sec / 3600u;
    uint32_t mins = (uptime_sec % 3600u) / 60u;
    uint32_t secs = uptime_sec % 60u;

    printf("\n=== NetworkD Statistics ===\n");
    printf("Uptime: %uh %um %us\n", hours, mins, secs);

    printf("\nIPv4:\n");
    printf("  RX: %llu packets, %llu bytes\n", 
           (unsigned long long)stats->ipv4.total_packets,
           (unsigned long long)stats->ipv4.total_bytes);
    printf("  ICMP: %llu, UDP: %llu, TCP: %llu, Other: %llu\n",
           (unsigned long long)stats->ipv4.icmp_packets,
           (unsigned long long)stats->ipv4.udp_packets,
           (unsigned long long)stats->ipv4.tcp_packets,
           (unsigned long long)stats->ipv4.other_packets);
    printf("  Errors: %llu, Checksum: %llu, Dropped: %llu\n",
           (unsigned long long)stats->ipv4.errors,
           (unsigned long long)stats->ipv4.checksum_errors,
           (unsigned long long)stats->ipv4.dropped);

    printf("\nARP:\n");
    printf("  Requests: %u, Replies: %u, Timeouts: %u\n",
           stats->arp.requests, stats->arp.replies, stats->arp.timeouts);
    printf("  Cache hits: %u, misses: %u\n",
           stats->arp.cache_hits, stats->arp.cache_misses);
    if (stats->arp.cache_hits + stats->arp.cache_misses > 0) {
        uint32_t total = stats->arp.cache_hits + stats->arp.cache_misses;
        uint32_t hit_rate = (stats->arp.cache_hits * 100u) / total;
        printf("  Hit rate: %u%%\n", hit_rate);
    }

    printf("\nDNS:\n");
    printf("  Queries: %u, Responses: %u, Timeouts: %u\n",
           stats->dns.queries, stats->dns.responses, stats->dns.timeouts);
    printf("  Cache hits: %u, misses: %u\n",
           stats->dns.cache_hits, stats->dns.cache_misses);
    if (stats->dns.cache_hits + stats->dns.cache_misses > 0) {
        uint32_t total = stats->dns.cache_hits + stats->dns.cache_misses;
        uint32_t hit_rate = (stats->dns.cache_hits * 100u) / total;
        printf("  Hit rate: %u%%\n", hit_rate);
    }

    printf("\nTCP:\n");
    printf("  Connections: %u (active: %u, failed: %u)\n",
           stats->tcp.connections, stats->tcp.active, stats->tcp.failed);
    printf("  RX: %llu bytes, TX: %llu bytes\n",
           (unsigned long long)stats->tcp.rx_bytes,
           (unsigned long long)stats->tcp.tx_bytes);
    printf("  Retransmits: %u, Timeouts: %u\n",
           stats->tcp.retransmits, stats->tcp.timeouts);

    printf("\nHTTP:\n");
    printf("  Requests: %u, Completed: %u, Failed: %u\n",
           stats->http.requests, stats->http.completed, stats->http.failed);
    printf("  Timeouts: %u, Redirects: %u\n",
           stats->http.timeouts, stats->http.redirects);

    printf("\n");
}

void netd_stats_ipv4_rx_packet(netd_stats_t* stats, uint32_t bytes, uint8_t proto) {
    if (!stats) {
        return;
    }

    stats->ipv4.total_packets++;
    stats->ipv4.total_bytes += bytes;

    switch (proto) {
    case 1:
        stats->ipv4.icmp_packets++;
        break;
    case 6:
        stats->ipv4.tcp_packets++;
        break;
    case 17:
        stats->ipv4.udp_packets++;
        break;
    default:
        stats->ipv4.other_packets++;
        break;
    }
}

void netd_stats_ipv4_tx_packet(netd_stats_t* stats, uint32_t bytes, uint8_t proto) {
    if (!stats) {
        return;
    }

    stats->ipv4.total_packets++;
    stats->ipv4.total_bytes += bytes;
}

void netd_stats_ipv4_error(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->ipv4.errors++;
}

void netd_stats_ipv4_checksum_error(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->ipv4.checksum_errors++;
}

void netd_stats_ipv4_dropped(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->ipv4.dropped++;
}

void netd_stats_arp_request(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->arp.requests++;
}

void netd_stats_arp_reply(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->arp.replies++;
}

void netd_stats_arp_timeout(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->arp.timeouts++;
}

void netd_stats_arp_cache_hit(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->arp.cache_hits++;
}

void netd_stats_arp_cache_miss(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->arp.cache_misses++;
}

void netd_stats_dns_query(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->dns.queries++;
}

void netd_stats_dns_response(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->dns.responses++;
}

void netd_stats_dns_timeout(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->dns.timeouts++;
}

void netd_stats_dns_cache_hit(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->dns.cache_hits++;
}

void netd_stats_dns_cache_miss(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->dns.cache_misses++;
}

void netd_stats_tcp_connection(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->tcp.connections++;
    stats->tcp.active++;
}

void netd_stats_tcp_close(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    if (stats->tcp.active > 0) {
        stats->tcp.active--;
    }
}

void netd_stats_tcp_failed(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->tcp.failed++;
    if (stats->tcp.active > 0) {
        stats->tcp.active--;
    }
}

void netd_stats_tcp_rx_bytes(netd_stats_t* stats, uint64_t bytes) {
    if (!stats) {
        return;
    }
    stats->tcp.rx_bytes += bytes;
}

void netd_stats_tcp_tx_bytes(netd_stats_t* stats, uint64_t bytes) {
    if (!stats) {
        return;
    }
    stats->tcp.tx_bytes += bytes;
}

void netd_stats_tcp_retransmit(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->tcp.retransmits++;
}

void netd_stats_tcp_timeout(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->tcp.timeouts++;
}

void netd_stats_http_request(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->http.requests++;
}

void netd_stats_http_completed(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->http.completed++;
}

void netd_stats_http_failed(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->http.failed++;
}

void netd_stats_http_timeout(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->http.timeouts++;
}

void netd_stats_http_redirect(netd_stats_t* stats) {
    if (!stats) {
        return;
    }
    stats->http.redirects++;
}

void netd_log(netd_ctx_t* ctx, int level, const char* fmt, ...) {
    if (!ctx || !fmt) {
        return;
    }

    if (level > ctx->log_level) {
        return;
    }

    const char* prefix = "";
    switch (level) {
    case NETD_LOG_LEVEL_ERROR:
        prefix = "[ERROR] ";
        break;
    case NETD_LOG_LEVEL_WARN:
        prefix = "[WARN]  ";
        break;
    case NETD_LOG_LEVEL_INFO:
        prefix = "[INFO]  ";
        break;
    case NETD_LOG_LEVEL_DEBUG:
        prefix = "[DEBUG] ";
        break;
    }

    printf("networkd: %s", prefix);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
}

void netd_iface_stats_update_rx(netd_iface_t* iface, uint32_t bytes, int error) {
    if (!iface) {
        return;
    }

    if (error) {
        iface->rx_errors++;
        return;
    }

    iface->rx_packets++;
    iface->rx_bytes += bytes;
}

void netd_iface_stats_update_tx(netd_iface_t* iface, uint32_t bytes, int error) {
    if (!iface) {
        return;
    }

    if (error) {
        iface->tx_errors++;
        return;
    }

    iface->tx_packets++;
    iface->tx_bytes += bytes;
}
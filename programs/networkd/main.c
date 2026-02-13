// SPDX-License-Identifier: GPL-2.0

#include <yula.h>

#include <net_ipc.h>

#include "netd_arp.h"
#include "netd_config.h"
#include "netd_device.h"
#include "netd_dns_cache.h"
#include "netd_http.h"
#include "netd_iface.h"
#include "netd_ipc.h"
#include "netd_stats.h"
#include "netd_tcp.h"
#include "netd_types.h"

static int netd_ctx_alloc_buffers(netd_ctx_t* ctx) {
    if (!ctx) {
        return -1;
    }

    ctx->rx_buf_size = NETD_JUMBO_FRAME_MAX;
    ctx->rx_buf = (uint8_t*)malloc(ctx->rx_buf_size);
    if (!ctx->rx_buf) {
        return -1;
    }

    ctx->tx_buf_size = NETD_JUMBO_FRAME_MAX;
    ctx->tx_buf = (uint8_t*)malloc(ctx->tx_buf_size);
    if (!ctx->tx_buf) {
        free(ctx->rx_buf);
        ctx->rx_buf = NULL;
        return -1;
    }

    return 0;
}

static void netd_ctx_free_buffers(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->rx_buf) {
        free(ctx->rx_buf);
        ctx->rx_buf = NULL;
    }

    if (ctx->tx_buf) {
        free(ctx->tx_buf);
        ctx->tx_buf = NULL;
    }

    ctx->rx_buf_size = 0;
    ctx->tx_buf_size = 0;
}

static int netd_ipc_ctx_init(netd_ipc_ctx_t* ipc) {
    if (!ipc) {
        return -1;
    }

    memset(ipc, 0, sizeof(*ipc));

    ipc->capacity = NETD_IPC_CLIENTS_INITIAL;
    ipc->clients = (netd_client_t*)calloc(ipc->capacity, sizeof(netd_client_t));
    if (!ipc->clients) {
        return -1;
    }

    for (uint32_t i = 0; i < ipc->capacity; i++) {
        ipc->clients[i].fd_in = -1;
        ipc->clients[i].fd_out = -1;
    }

    return 0;
}

static void netd_ipc_ctx_free(netd_ipc_ctx_t* ipc) {
    if (!ipc || !ipc->clients) {
        return;
    }

    for (uint32_t i = 0; i < ipc->count; i++) {
        netd_client_t* c = &ipc->clients[i];
        if (c->used) {
            if (c->fd_in >= 0) {
                close(c->fd_in);
            }
            if (c->fd_out >= 0 && c->fd_out != c->fd_in) {
                close(c->fd_out);
            }
        }
    }

    free(ipc->clients);
    ipc->clients = NULL;
    ipc->count = 0;
    ipc->capacity = 0;
}

static int netd_dns_waits_init(netd_dns_wait_mgr_t* mgr) {
    if (!mgr) {
        return -1;
    }

    memset(mgr, 0, sizeof(*mgr));

    mgr->capacity = NETD_DNS_MAX_WAITS;
    mgr->slots = (netd_dns_wait_slot_t*)calloc(mgr->capacity, sizeof(netd_dns_wait_slot_t));
    if (!mgr->slots) {
        return -1;
    }

    return 0;
}

static void netd_dns_waits_free(netd_dns_wait_mgr_t* mgr) {
    if (!mgr) {
        return;
    }

    if (mgr->slots) {
        free(mgr->slots);
        mgr->slots = NULL;
    }

    mgr->count = 0;
    mgr->capacity = 0;
}

static void netd_ctx_init(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->iface.fd = -1;
    ctx->iface.ip = NETD_DEFAULT_IP;
    ctx->iface.mask = NETD_DEFAULT_MASK;
    ctx->iface.gw = NETD_DEFAULT_GW;
    ctx->iface.mtu = NETD_FRAME_MAX;
    ctx->dns_server = NETD_DEFAULT_DNS;

    ctx->log_level = NETD_LOG_LEVEL;
    ctx->enable_stats = NETD_STATS_ENABLED;

    netd_rand_init(&ctx->rand);

    if (netd_ctx_alloc_buffers(ctx) != 0) {
        printf("networkd: failed to allocate I/O buffers\n");
    }

    if (netd_ipc_ctx_init(&ctx->ipc) != 0) {
        printf("networkd: failed to initialize IPC context\n");
    }

    if (netd_dns_waits_init(&ctx->dns_waits) != 0) {
        printf("networkd: failed to initialize DNS wait manager\n");
    }

    netd_dns_cache_init(&ctx->dns_cache);
    netd_stats_init(&ctx->stats);
}

static void netd_ctx_cleanup(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    netd_log_info(ctx, "Shutting down network daemon");

    if (ctx->enable_stats) {
        netd_stats_print(&ctx->stats);
    }

    netd_tcp_shutdown(ctx);
    netd_iface_close(ctx);
    netd_arp_cleanup(ctx);
    netd_dns_cache_cleanup(&ctx->dns_cache);
    netd_dns_waits_free(&ctx->dns_waits);
    netd_ipc_ctx_free(&ctx->ipc);
    netd_ctx_free_buffers(ctx);
}

static int netd_ipc_ensure_capacity(netd_ipc_ctx_t* ipc, uint32_t needed) {
    if (!ipc) {
        return 0;
    }

    if (ipc->capacity >= needed) {
        return 1;
    }

    if (needed > NETD_IPC_CLIENTS_MAX) {
        return 0;
    }

    uint32_t new_cap = ipc->capacity;
    while (new_cap < needed && new_cap < NETD_IPC_CLIENTS_MAX) {
        new_cap *= 2;
    }

    if (new_cap > NETD_IPC_CLIENTS_MAX) {
        new_cap = NETD_IPC_CLIENTS_MAX;
    }

    netd_client_t* new_clients = (netd_client_t*)realloc(
        ipc->clients,
        new_cap * sizeof(netd_client_t)
    );

    if (!new_clients) {
        return 0;
    }

    if (new_cap > ipc->capacity) {
        memset(
            new_clients + ipc->capacity,
            0,
            (new_cap - ipc->capacity) * sizeof(netd_client_t)
        );
        for (uint32_t i = ipc->capacity; i < new_cap; i++) {
            new_clients[i].fd_in = -1;
            new_clients[i].fd_out = -1;
        }
    }

    ipc->clients = new_clients;
    ipc->capacity = new_cap;

    return 1;
}

static uint32_t netd_build_poll_fds(netd_ctx_t* ctx, int listen_fd, pollfd_t** out_fds) {
    if (!ctx || !out_fds) {
        return 0;
    }

    netd_ipc_ctx_t* ipc = &ctx->ipc;
    uint32_t max_fds = 1 + ipc->count;

    pollfd_t* fds = (pollfd_t*)calloc(max_fds, sizeof(pollfd_t));
    if (!fds) {
        *out_fds = NULL;
        return 0;
    }

    uint32_t nfds = 0;

    fds[nfds].fd = listen_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    nfds++;

    for (uint32_t i = 0; i < ipc->count; i++) {
        netd_client_t* c = &ipc->clients[i];
        if (!c->used || c->fd_in < 0) {
            continue;
        }

        if (nfds >= max_fds) {
            break;
        }

        fds[nfds].fd = c->fd_in;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    *out_fds = fds;
    return nfds;
}

static void netd_accept_clients(netd_ctx_t* ctx, int listen_fd) {
    if (!ctx) {
        return;
    }

    netd_ipc_ctx_t* ipc = &ctx->ipc;

    for (;;) {
        int out_fds[2];
        int acc = ipc_accept(listen_fd, out_fds);
        if (acc <= 0) {
            break;
        }

        uint32_t free_slot = ipc->count;
        for (uint32_t i = 0; i < ipc->count; i++) {
            if (!ipc->clients[i].used) {
                free_slot = i;
                break;
            }
        }

        if (free_slot >= ipc->capacity) {
            if (!netd_ipc_ensure_capacity(ipc, free_slot + 1)) {
                netd_log_warn(ctx, "IPC client limit reached, rejecting connection");
                close(out_fds[0]);
                close(out_fds[1]);
                continue;
            }
        }

        netd_client_t* c = &ipc->clients[free_slot];
        c->used = 1;
        c->fd_in = out_fds[0];
        c->fd_out = out_fds[1];
        c->req_count = 0;
        c->last_activity_ms = uptime_ms();
        net_ipc_rx_reset(&c->rx);

        if (free_slot >= ipc->count) {
            ipc->count = free_slot + 1;
        }

        netd_log_debug(ctx, "IPC client connected (slot %u, total: %u)", 
                       free_slot, ipc->count);
    }
}

static void netd_cleanup_idle_clients(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    netd_ipc_ctx_t* ipc = &ctx->ipc;
    uint32_t now = uptime_ms();
    uint32_t timeout = 300000;

    for (uint32_t i = 0; i < ipc->count; i++) {
        netd_client_t* c = &ipc->clients[i];
        if (!c->used) {
            continue;
        }

        uint32_t idle = now - c->last_activity_ms;
        if (idle > timeout) {
            netd_log_debug(ctx, "Closing idle IPC client (slot %u, idle: %ums)", 
                          i, idle);
            
            if (c->fd_in >= 0) {
                close(c->fd_in);
            }
            if (c->fd_out >= 0 && c->fd_out != c->fd_in) {
                close(c->fd_out);
            }
            
            c->used = 0;
            c->fd_in = -1;
            c->fd_out = -1;
        }
    }

    while (ipc->count > 0 && !ipc->clients[ipc->count - 1].used) {
        ipc->count--;
    }
}

static void netd_periodic_tasks(netd_ctx_t* ctx) {
    static uint32_t last_cleanup_ms = 0;
    static uint32_t last_stats_ms = 0;

    if (!ctx) {
        return;
    }

    uint32_t now = uptime_ms();

    if (now - last_cleanup_ms > 30000) {
        netd_cleanup_idle_clients(ctx);
        netd_tcp_cleanup_idle(ctx);
        last_cleanup_ms = now;
    }

    if (ctx->enable_stats && now - last_stats_ms > 60000) {
        netd_log_debug(ctx, "Active connections: TCP=%u, IPC=%u, ARP cache=%u, DNS cache=%u",
                      ctx->tcp.active_connections,
                      ctx->ipc.count,
                      netd_arp_cache_size(ctx),
                      netd_dns_cache_size(&ctx->dns_cache));
        last_stats_ms = now;
    }

    netd_iface_periodic(ctx);
}

int main(void) {
    netd_ctx_t ctx;
    netd_ctx_init(&ctx);

    netd_tcp_init(&ctx);

    if (netd_iface_ensure_up(&ctx) != 0) {
        netd_log_warn(&ctx, "Failed to open /dev/ne2k0, will retry");
    }

    netd_links_init(&ctx);
    netd_arp_init(&ctx);
    netd_iface_print_state(&ctx);

    int listen_fd = ipc_listen("networkd");
    if (listen_fd < 0) {
        netd_log_error(&ctx, "ipc_listen(networkd) failed");
        netd_ctx_cleanup(&ctx);
        return 1;
    }

    uint32_t iteration = 0;
    uint32_t last_periodic = 0;

    for (;;) {
        iteration++;

        pollfd_t* fds = NULL;
        uint32_t nfds = netd_build_poll_fds(&ctx, listen_fd, &fds);

        if (!fds || nfds == 0) {
            if (fds) {
                free(fds);
            }
            sleep(NETD_POLL_TIMEOUT_MS);
            continue;
        }

        int poll_result = poll(fds, nfds, NETD_POLL_TIMEOUT_MS);

        if (poll_result > 0) {
            if (fds[0].revents & POLLIN) {
                netd_accept_clients(&ctx, listen_fd);
            }
        }

        free(fds);

        netd_device_process(&ctx);
        netd_ipc_process_clients(&ctx, ctx.ipc.clients, ctx.ipc.count);
        netd_http_tick(&ctx);

        uint32_t now = uptime_ms();
        if (now - last_periodic > 1000) {
            netd_periodic_tasks(&ctx);
            last_periodic = now;
        }

        if (iteration % 10000 == 0) {
            netd_log_debug(&ctx, "Heartbeat: iteration=%u, uptime=%us",
                          iteration, (now - ctx.stats.start_time_ms) / 1000);
        }
    }

    close(listen_fd);
    netd_ctx_cleanup(&ctx);

    return 0;
}
// SPDX-License-Identifier: GPL-2.0

#include <yula.h>

#include <net_ipc.h>

#include "netd_arp.h"
#include "netd_device.h"
#include "netd_iface.h"
#include "netd_http.h"
#include "netd_ipc.h"
#include "netd_tcp.h"
#include "netd_types.h"

static void netd_ctx_init(netd_ctx_t* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->iface.fd = -1;
    ctx->iface.ip = NETD_DEFAULT_IP;
    ctx->iface.mask = NETD_DEFAULT_MASK;
    ctx->iface.gw = NETD_DEFAULT_GW;
    ctx->dns_server = NETD_DEFAULT_DNS;
    netd_rand_init(&ctx->rand);
}

static uint32_t netd_build_poll_fds(int listen_fd, const netd_client_t clients[NETD_MAX_CLIENTS], pollfd_t fds[1 + NETD_MAX_CLIENTS]) {
    uint32_t nfds = 0;

    fds[nfds].fd = listen_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    nfds++;

    for (int i = 0; i < NETD_MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            continue;
        }

        fds[nfds].fd = clients[i].fd_in;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    return nfds;
}

int main(void) {
    netd_ctx_t ctx;
    netd_ctx_init(&ctx);
    netd_tcp_init(&ctx);

    if (netd_iface_ensure_up(&ctx) != 0) {
        printf("networkd: failed to open /dev/ne2k0\n");
    }

    netd_links_init(&ctx);
    netd_arp_init(&ctx);
    netd_iface_print_state(&ctx);

    int listen_fd = ipc_listen("networkd");
    if (listen_fd < 0) {
        printf("networkd: ipc_listen(networkd) failed\n");
        return 1;
    }

    netd_client_t clients[NETD_MAX_CLIENTS];
    netd_ipc_clients_init(clients);

    for (;;) {
        pollfd_t fds[1 + NETD_MAX_CLIENTS];
        uint32_t nfds = netd_build_poll_fds(listen_fd, clients, fds);

        (void)poll(fds, nfds, 50);

        if (fds[0].revents & POLLIN) {
            netd_ipc_accept_pending(listen_fd, clients);
        }

        netd_device_process(&ctx);
        netd_ipc_process_clients(&ctx, clients);
        netd_http_tick(&ctx);
        netd_iface_periodic(&ctx);
    }

    netd_tcp_shutdown(&ctx);
    return 0;
}

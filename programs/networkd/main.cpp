#include "netdev.h"
#include "arp.h"
#include "ipv4_icmp.h"
#include "ipc_server.h"
#include "arena.h"
#include "net_spsc.h"
#include "netd_msgs.h"

#include <yula.h>

namespace {

static uint32_t ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint32_t ip = ((uint32_t)a << 24) |
                        ((uint32_t)b << 16) |
                        ((uint32_t)c << 8) |
                        (uint32_t)d;

    return netd::htonl(ip);
}

struct IpcThreadCtx {
    netd::IpcServer* ipc;
    int notify_fd_r;
};

static void* ipc_thread_main(void* arg) {
    IpcThreadCtx* ctx = (IpcThreadCtx*)arg;
    if (!ctx || !ctx->ipc) {
        return nullptr;
    }

    for (;;) {
        (void)ctx->ipc->wait(ctx->notify_fd_r, 1000);

        const uint32_t now = uptime_ms();
        ctx->ipc->step(now);
    }
}

static void print_mac(const netd::Mac& mac) {
    printf(
        "%02x:%02x:%02x:%02x:%02x:%02x",
        mac.b[0],
        mac.b[1],
        mac.b[2],
        mac.b[3],
        mac.b[4],
        mac.b[5]
    );
}

}

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    netd::Arena arena;
    if (!arena.init(128u * 1024u)) {
        printf("networkd: arena init failed\n");
        return 1;
    }

    netd::NetDev dev;
    if (!dev.open_default()) {
        printf("networkd: failed to open /dev/ne2k0\n");
        return 1;
    }

    const netd::Mac mac = dev.mac();

    netd::Arp arp(arena, dev);

    netd::ArpConfig arp_cfg{};
    arp_cfg.ip_be = ip_be(10, 0, 2, 15);
    arp_cfg.mac = mac;
    arp.set_config(arp_cfg);

    netd::Ipv4Icmp ip(arena, dev, arp);

    netd::IpConfig ip_cfg{};
    ip_cfg.ip_be = arp_cfg.ip_be;
    ip_cfg.mask_be = ip_be(255, 255, 255, 0);
    ip_cfg.gw_be = ip_be(10, 0, 2, 2);
    ip.set_config(ip_cfg);

    netd::SpscQueue<netd::PingSubmitMsg, 256> ipc_to_core;
    netd::SpscQueue<netd::PingResultMsg, 256> core_to_ipc;

    int notify_fds[2] = { -1, -1 };
    if (pipe(notify_fds) != 0) {
        printf("networkd: pipe failed\n");
        return 1;
    }

    netd::IpcServer ipc(arena, ipc_to_core, core_to_ipc);
    if (!ipc.listen()) {
        printf("networkd: ipc_listen failed\n");
        return 1;
    }

    pthread_t ipc_thread{};
    IpcThreadCtx ipc_ctx{};
    ipc_ctx.ipc = &ipc;
    ipc_ctx.notify_fd_r = notify_fds[0];

    if (pthread_create(&ipc_thread, nullptr, ipc_thread_main, &ipc_ctx) != 0) {
        printf("networkd: pthread_create failed\n");
        return 1;
    }

    printf("networkd: iface ne2k0 up\n");
    printf("networkd: mac ");
    print_mac(mac);
    printf("\n");

    printf("networkd: ip 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2\n");

    {
        netd::Mac gw_mac{};
        (void)arp.resolve(ip_cfg.gw_be, gw_mac, 2000u);
    }

    uint8_t frame[1600];

    for (;;) {
        const uint32_t now = uptime_ms();

        pollfd_t fds[1];
        fds[0].fd = dev.fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        (void)poll(fds, 1, 10);

        for (;;) {
            const int r = dev.read_frame(frame, (uint32_t)sizeof(frame));
            if (r <= 0) {
                break;
            }

            const uint32_t flen = (uint32_t)r;
            const netd::EthHdr* eth = (const netd::EthHdr*)frame;
            const uint16_t et = netd::ntohs(eth->ethertype);

            if (et == netd::ETHERTYPE_ARP) {
                (void)arp.handle_frame(frame, flen, now);
                continue;
            }

            if (et == netd::ETHERTYPE_IPV4) {
                (void)ip.handle_frame(frame, flen, now);
                continue;
            }
        }

        for (;;) {
            netd::PingSubmitMsg msg{};
            if (!ipc_to_core.pop(msg)) {
                break;
            }

            netd::Ipv4Icmp::PingRequest pr{};
            pr.dst_ip_be = msg.dst_ip_be;
            pr.ident_be = msg.ident_be;
            pr.seq_be = msg.seq_be;
            pr.timeout_ms = msg.timeout_ms;
            pr.tag = msg.tag;
            pr.client_fd_w = msg.client_fd_w;

            (void)ip.submit_ping(pr, now);
        }

        ip.step(now);

        for (;;) {
            netd::Ipv4Icmp::PingResult r{};
            if (!ip.poll_result(r)) {
                break;
            }

            netd::PingResultMsg msg{};
            msg.dst_ip_be = r.dst_ip_be;
            msg.ident_be = r.ident_be;
            msg.seq_be = r.seq_be;
            msg.rtt_ms = r.rtt_ms;
            msg.ok = r.ok;
            msg.tag = r.tag;
            msg.client_fd_w = r.client_fd_w;

            if (core_to_ipc.push(msg)) {
                uint8_t b = 1u;
                (void)pipe_try_write(notify_fds[1], &b, 1u);
            }
        }
    }

    return 0;
}

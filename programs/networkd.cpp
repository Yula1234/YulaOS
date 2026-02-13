#include "networkd/netdev.h"
#include "networkd/arp.h"
#include "networkd/ipv4_icmp.h"
#include "networkd/ipc_server.h"

#include <yula.h>

namespace {

static uint32_t ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return netd::htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d);
}

static void print_mac(const netd::Mac& mac) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           mac.b[0], mac.b[1], mac.b[2], mac.b[3], mac.b[4], mac.b[5]);
}

}

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    netd::NetDev dev;
    if (!dev.open_default()) {
        printf("networkd: failed to open /dev/ne2k0\n");
        return 1;
    }

    const netd::Mac mac = dev.mac();

    netd::Arp arp(dev);
    netd::ArpConfig arp_cfg{};
    arp_cfg.ip_be = ip_be(10, 0, 2, 15);
    arp_cfg.mac = mac;
    arp.set_config(arp_cfg);

    netd::Ipv4Icmp ip(dev, arp);
    netd::IpConfig ip_cfg{};
    ip_cfg.ip_be = arp_cfg.ip_be;
    ip_cfg.mask_be = ip_be(255, 255, 255, 0);
    ip_cfg.gw_be = ip_be(10, 0, 2, 2);
    ip.set_config(ip_cfg);

    netd::IpcServer ipc(ip);
    if (!ipc.listen()) {
        printf("networkd: ipc_listen failed\n");
        return 1;
    }

    printf("networkd: iface ne2k0 up\n");
    printf("networkd: mac ");
    print_mac(mac);
    printf("\n");

    printf("networkd: ip 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2\n");

    uint8_t frame[1600];

    for (;;) {
        const uint32_t now = uptime_ms();

        pollfd_t fds[2];
        fds[0].fd = dev.fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = ipc.listen_fd();
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        (void)poll(fds, 2, 10);

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

        ipc.step(now);
    }

    return 0;
}

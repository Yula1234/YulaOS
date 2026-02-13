#include "netdev.h"
#include "arp.h"
#include "ipv4_icmp.h"
#include "ipc_server.h"
#include "arena.h"
#include "net_spsc.h"
#include "net_channel.h"
#include "netd_msgs.h"
#include "net_dispatch.h"

#include <yula.h>

namespace {

static uint32_t ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint32_t ip = ((uint32_t)a << 24) |
                        ((uint32_t)b << 16) |
                        ((uint32_t)c << 8) |
                        (uint32_t)d;

    return netd::htonl(ip);
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

static void handle_arp(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    netd::Arp* arp = (netd::Arp*)ctx;
    if (!arp) {
        return;
    }

    (void)arp->handle_frame(frame, len, now_ms);
}

static void handle_ipv4(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    netd::Ipv4Icmp* ip = (netd::Ipv4Icmp*)ctx;
    if (!ip) {
        return;
    }

    (void)ip->handle_frame(frame, len, now_ms);
}

class NetdApp {
public:
    NetdApp();

    NetdApp(const NetdApp&) = delete;
    NetdApp& operator=(const NetdApp&) = delete;

    bool init();
    int run();

private:
    struct IpcThreadCtx {
        netd::IpcServer* ipc;
        const netd::PipePair* notify;
    };

    static void* ipc_thread_main(void* arg);

    static uint32_t default_ip_be();
    static uint32_t default_mask_be();
    static uint32_t default_gw_be();

    netd::Arena m_core_arena;
    netd::Arena m_ipc_arena;

    netd::NetDev m_dev;

    netd::SpscQueue<netd::PingSubmitMsg, 256> m_ipc_to_core_q;
    netd::SpscQueue<netd::PingResultMsg, 256> m_core_to_ipc_q;

    netd::PipePair m_core_to_ipc_notify;
    netd::PipePair m_ipc_to_core_notify;

    netd::SpscChannel<netd::PingSubmitMsg, 256> m_ipc_to_core_chan;
    netd::SpscChannel<netd::PingResultMsg, 256> m_core_to_ipc_chan;

    alignas(netd::Arp) uint8_t m_arp_storage[sizeof(netd::Arp)];
    netd::Arp* m_arp;

    alignas(netd::Ipv4Icmp) uint8_t m_ip_storage[sizeof(netd::Ipv4Icmp)];
    netd::Ipv4Icmp* m_ip;

    alignas(netd::EthertypeDispatch) uint8_t m_eth_storage[sizeof(netd::EthertypeDispatch)];
    netd::EthertypeDispatch* m_eth_dispatch;

    alignas(netd::IpcServer) uint8_t m_ipc_storage[sizeof(netd::IpcServer)];
    netd::IpcServer* m_ipc;

    pthread_t m_ipc_thread;
    IpcThreadCtx m_ipc_ctx;
};

NetdApp::NetdApp()
    : m_core_arena(),
      m_ipc_arena(),
      m_dev(),
      m_ipc_to_core_q(),
      m_core_to_ipc_q(),
      m_core_to_ipc_notify(),
      m_ipc_to_core_notify(),
      m_ipc_to_core_chan(m_ipc_to_core_q, m_ipc_to_core_notify),
      m_core_to_ipc_chan(m_core_to_ipc_q, m_core_to_ipc_notify),
      m_arp_storage{},
      m_arp(nullptr),
      m_ip_storage{},
      m_ip(nullptr),
      m_eth_storage{},
      m_eth_dispatch(nullptr),
      m_ipc_storage{},
      m_ipc(nullptr),
      m_ipc_thread{},
      m_ipc_ctx{} {
}

uint32_t NetdApp::default_ip_be() {
    return ip_be(10, 0, 2, 15);
}

uint32_t NetdApp::default_mask_be() {
    return ip_be(255, 255, 255, 0);
}

uint32_t NetdApp::default_gw_be() {
    return ip_be(10, 0, 2, 2);
}

void* NetdApp::ipc_thread_main(void* arg) {
    IpcThreadCtx* ctx = (IpcThreadCtx*)arg;
    if (!ctx || !ctx->ipc || !ctx->notify) {
        return nullptr;
    }

    for (;;) {
        (void)ctx->ipc->wait(*ctx->notify, -1);

        const uint32_t now = uptime_ms();
        ctx->ipc->step(now);
    }
}

bool NetdApp::init() {
    if (!m_core_arena.init(256u * 1024u)) {
        printf("networkd: arena init failed\n");
        return false;
    }

    if (!m_ipc_arena.init(128u * 1024u)) {
        printf("networkd: arena init failed\n");
        return false;
    }

    if (!m_dev.open_default()) {
        printf("networkd: failed to open /dev/ne2k0\n");
        return false;
    }

    m_arp = new (m_arp_storage) netd::Arp(m_core_arena, m_dev);
    m_ip = new (m_ip_storage) netd::Ipv4Icmp(m_core_arena, m_dev, *m_arp);
    m_eth_dispatch = new (m_eth_storage) netd::EthertypeDispatch(m_core_arena);

    if (!m_core_to_ipc_notify.create()) {
        printf("networkd: pipe failed\n");
        return false;
    }

    if (!m_ipc_to_core_notify.create()) {
        printf("networkd: pipe failed\n");
        return false;
    }

    const netd::Mac mac = m_dev.mac();

    netd::ArpConfig arp_cfg{};
    arp_cfg.ip_be = default_ip_be();
    arp_cfg.mac = mac;
    m_arp->set_config(arp_cfg);

    netd::IpConfig ip_cfg{};
    ip_cfg.ip_be = arp_cfg.ip_be;
    ip_cfg.mask_be = default_mask_be();
    ip_cfg.gw_be = default_gw_be();
    m_ip->set_config(ip_cfg);

    (void)m_eth_dispatch->reserve(8u);
    (void)m_eth_dispatch->add(netd::ETHERTYPE_ARP, m_arp, &handle_arp);
    (void)m_eth_dispatch->add(netd::ETHERTYPE_IPV4, m_ip, &handle_ipv4);

    m_ipc = new (m_ipc_storage) netd::IpcServer(m_ipc_arena, m_ipc_to_core_chan, m_core_to_ipc_q);
    if (!m_ipc->listen()) {
        printf("networkd: ipc_listen failed\n");
        return false;
    }

    m_ipc_ctx.ipc = m_ipc;
    m_ipc_ctx.notify = &m_core_to_ipc_notify;

    if (pthread_create(&m_ipc_thread, nullptr, &NetdApp::ipc_thread_main, &m_ipc_ctx) != 0) {
        printf("networkd: pthread_create failed\n");
        return false;
    }

    return true;
}

int NetdApp::run() {
    const netd::Mac mac = m_dev.mac();

    printf("networkd: iface ne2k0 up\n");
    printf("networkd: mac ");
    print_mac(mac);
    printf("\n");

    printf("networkd: ip 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2\n");

    {
        netd::Mac gw_mac{};
        (void)m_arp->resolve(default_gw_be(), gw_mac, 2000u);
    }

    uint8_t frame[1600];

    for (;;) {
        const uint32_t now = uptime_ms();

        pollfd_t fds[2];

        fds[0].fd = m_dev.fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = m_ipc_to_core_chan.notify_fd();
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        (void)poll(fds, 2, 10);

        if ((fds[1].revents & POLLIN) != 0) {
            m_ipc_to_core_chan.drain_notify();
        }

        for (;;) {
            const int r = m_dev.read_frame(frame, (uint32_t)sizeof(frame));
            if (r <= 0) {
                break;
            }

            const uint32_t flen = (uint32_t)r;
            const netd::EthHdr* eth = (const netd::EthHdr*)frame;
            const uint16_t et = netd::ntohs(eth->ethertype);

            (void)m_eth_dispatch->dispatch(et, frame, flen, now);
        }

        for (;;) {
            netd::PingSubmitMsg msg{};
            if (!m_ipc_to_core_q.pop(msg)) {
                break;
            }

            netd::Ipv4Icmp::PingRequest pr{};
            pr.dst_ip_be = msg.dst_ip_be;
            pr.ident_be = msg.ident_be;
            pr.seq_be = msg.seq_be;
            pr.timeout_ms = msg.timeout_ms;
            pr.tag = msg.tag;
            pr.client_token = msg.client_token;

            (void)m_ip->submit_ping(pr, now);
        }

        m_ip->step(now);

        for (;;) {
            netd::Ipv4Icmp::PingResult r{};
            if (!m_ip->poll_result(r)) {
                break;
            }

            netd::PingResultMsg msg{};
            msg.dst_ip_be = r.dst_ip_be;
            msg.ident_be = r.ident_be;
            msg.seq_be = r.seq_be;
            msg.rtt_ms = r.rtt_ms;
            msg.ok = r.ok;
            msg.tag = r.tag;
            msg.client_token = r.client_token;

            (void)m_core_to_ipc_chan.push_and_wake(msg);
        }
    }

    return 0;
}

}

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    NetdApp app;
    if (!app.init()) {
        return 1;
    }

    return app.run();
}

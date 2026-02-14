#include "netdev.h"
#include "arp.h"
#include "ipv4_icmp.h"
#include "ipc_server.h"
#include "dns_client.h"
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

    bool init_arenas();
    bool init_device();
    bool init_protocol_stack();
    bool init_ipc();

    void poll_once(uint32_t now_ms);
    void drain_core_requests(uint32_t now_ms);
    void publish_events(uint32_t now_ms);

    netd::Arena m_core_arena;
    netd::Arena m_ipc_arena;

    netd::NetDev m_dev;

    netd::SpscQueue<netd::CoreReqMsg, 256> m_ipc_to_core_q;
    netd::SpscQueue<netd::CoreEvtMsg, 256> m_core_to_ipc_q;

    netd::PipePair m_core_to_ipc_notify;
    netd::PipePair m_ipc_to_core_notify;

    netd::SpscChannel<netd::CoreReqMsg, 256> m_ipc_to_core_chan;
    netd::SpscChannel<netd::CoreEvtMsg, 256> m_core_to_ipc_chan;

    alignas(netd::Arp) uint8_t m_arp_storage[sizeof(netd::Arp)];
    netd::Arp* m_arp;

    alignas(netd::Ipv4Icmp) uint8_t m_ip_storage[sizeof(netd::Ipv4Icmp)];
    netd::Ipv4Icmp* m_ip;

    alignas(netd::DnsClient) uint8_t m_dns_storage[sizeof(netd::DnsClient)];
    netd::DnsClient* m_dns;

    alignas(netd::EthertypeDispatch) uint8_t m_eth_storage[sizeof(netd::EthertypeDispatch)];
    netd::EthertypeDispatch* m_eth_dispatch;

    alignas(netd::IpcServer) uint8_t m_ipc_storage[sizeof(netd::IpcServer)];
    netd::IpcServer* m_ipc;

    pthread_t m_ipc_thread;
    IpcThreadCtx m_ipc_ctx;
};

void NetdApp::poll_once(uint32_t now_ms) {
    (void)now_ms;

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
}

void NetdApp::drain_core_requests(uint32_t now_ms) {
    for (;;) {
        netd::CoreReqMsg req{};
        if (!m_ipc_to_core_q.pop(req)) {
            break;
        }

        if (req.type == netd::CoreReqType::PingSubmit) {
            const netd::PingSubmitMsg& msg = req.ping;

            netd::Ipv4Icmp::PingRequest pr{};
            pr.dst_ip_be = msg.dst_ip_be;
            pr.ident_be = msg.ident_be;
            pr.seq_be = msg.seq_be;
            pr.timeout_ms = msg.timeout_ms;
            pr.tag = msg.tag;
            pr.client_token = msg.client_token;

            (void)m_ip->submit_ping(pr, now_ms);
            continue;
        }

        if (req.type == netd::CoreReqType::DnsResolveSubmit) {
            const netd::DnsResolveSubmitMsg& msg = req.dns;

            netd::ResolveRequest dr{};
            dr.name_len = msg.name_len;
            memcpy(dr.name, msg.name, msg.name_len);
            dr.tag = msg.tag;
            dr.client_token = msg.client_token;
            dr.timeout_ms = msg.timeout_ms;

            (void)m_dns->submit_resolve(dr, now_ms);
            continue;
        }
    }
}

void NetdApp::publish_events(uint32_t now_ms) {
    for (;;) {
        netd::Ipv4Icmp::PingResult r{};
        if (!m_ip->poll_result(r)) {
            break;
        }

        netd::CoreEvtMsg evt{};
        evt.type = netd::CoreEvtType::PingResult;
        evt.ping.dst_ip_be = r.dst_ip_be;
        evt.ping.ident_be = r.ident_be;
        evt.ping.seq_be = r.seq_be;
        evt.ping.rtt_ms = r.rtt_ms;
        evt.ping.ok = r.ok;
        evt.ping.tag = r.tag;
        evt.ping.client_token = r.client_token;

        (void)m_core_to_ipc_chan.push_and_wake(evt);
    }

    for (;;) {
        netd::ResolveResult r{};
        if (!m_dns->poll_result(r)) {
            break;
        }

        netd::CoreEvtMsg evt{};
        evt.type = netd::CoreEvtType::DnsResolveResult;
        evt.dns.ip_be = r.ip_be;
        evt.dns.ok = r.ok;
        evt.dns.tag = r.tag;
        evt.dns.client_token = r.client_token;

        (void)m_core_to_ipc_chan.push_and_wake(evt);
    }
}

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
      m_dns_storage{},
      m_dns(nullptr),
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

static uint32_t default_dns_be() {
    return ip_be(8, 8, 8, 8);
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
    if (!init_arenas()) {
        return false;
    }

    if (!init_device()) {
        return false;
    }

    if (!init_protocol_stack()) {
        return false;
    }

    if (!init_ipc()) {
        return false;
    }

    return true;
}

bool NetdApp::init_arenas() {
    if (!m_core_arena.init(256u * 1024u)) {
        printf("networkd: arena init failed\n");
        return false;
    }

    if (!m_ipc_arena.init(128u * 1024u)) {
        printf("networkd: arena init failed\n");
        return false;
    }

    return true;
}

bool NetdApp::init_device() {
    if (!m_dev.open_default()) {
        printf("networkd: failed to open /dev/ne2k0\n");
        return false;
    }

    return true;
}

bool NetdApp::init_protocol_stack() {
    m_arp = new (m_arp_storage) netd::Arp(m_core_arena, m_dev);
    m_ip = new (m_ip_storage) netd::Ipv4Icmp(m_core_arena, m_dev, *m_arp);
    m_dns = new (m_dns_storage) netd::DnsClient(m_core_arena, m_dev, *m_arp);
    m_eth_dispatch = new (m_eth_storage) netd::EthertypeDispatch(m_core_arena);

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

    netd::DnsConfig dns_cfg{};
    dns_cfg.ip_be = ip_cfg.ip_be;
    dns_cfg.gw_be = ip_cfg.gw_be;
    dns_cfg.dns_ip_be = default_dns_be();
    m_dns->set_config(dns_cfg);

    (void)m_ip->add_proto_handler(netd::IP_PROTO_UDP, m_dns, &netd::DnsClient::udp_proto_handler);

    (void)m_eth_dispatch->reserve(8u);
    (void)m_eth_dispatch->add(netd::ETHERTYPE_ARP, m_arp, &handle_arp);
    (void)m_eth_dispatch->add(netd::ETHERTYPE_IPV4, m_ip, &handle_ipv4);

    return true;
}

bool NetdApp::init_ipc() {
    if (!m_core_to_ipc_notify.create()) {
        printf("networkd: pipe failed\n");
        return false;
    }

    if (!m_ipc_to_core_notify.create()) {
        printf("networkd: pipe failed\n");
        return false;
    }

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

        poll_once(now);

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

        drain_core_requests(now);

        m_ip->step(now);
        m_dns->step(now);

        publish_events(now);
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

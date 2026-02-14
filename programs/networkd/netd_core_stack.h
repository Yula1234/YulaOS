#ifndef YOS_NETD_CORE_STACK_H
#define YOS_NETD_CORE_STACK_H

#include "arena.h"
#include "netdev.h"
#include "arp.h"
#include "ipv4_icmp.h"
#include "dns_client.h"
#include "net_dispatch.h"
#include "net_inplace.h"

namespace netd {

class NetdCoreStack {
public:
    NetdCoreStack(Arena& arena, NetDev& dev);

    NetdCoreStack(const NetdCoreStack&) = delete;
    NetdCoreStack& operator=(const NetdCoreStack&) = delete;

    bool init();

    void step(uint32_t now_ms);

    bool handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms);

    bool submit_ping(const Ipv4Icmp::PingRequest& req, uint32_t now_ms);
    bool poll_ping_result(Ipv4Icmp::PingResult& out);

    bool submit_resolve(const ResolveRequest& req, uint32_t now_ms);
    bool poll_resolve_result(ResolveResult& out);

    bool try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const;

    bool resolve_gateway(uint32_t gw_ip_be, Mac& out_mac, uint32_t timeout_ms);

    Mac mac() const;

    uint32_t default_ip_be() const;
    uint32_t default_mask_be() const;
    uint32_t default_gw_be() const;
    uint32_t default_dns_be() const;

private:
    static void handle_arp(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms);
    static void handle_ipv4(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms);

    static uint32_t ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

    Arena& m_arena;
    NetDev& m_dev;

    Inplace<Arp> m_arp;
    Inplace<Ipv4Icmp> m_ip;
    Inplace<DnsClient> m_dns;
    Inplace<EthertypeDispatch> m_eth_dispatch;
};

}

#endif

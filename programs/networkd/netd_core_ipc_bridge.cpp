#include "netd_core_ipc_bridge.h"

#include <string.h>

namespace netd {

NetdCoreIpcBridge::NetdCoreIpcBridge(
    NetdCoreStack& stack,
    SpscQueue<CoreReqMsg, 256>& req_q,
    SpscChannel<CoreReqMsg, 256>& req_chan,
    SpscChannel<CoreEvtMsg, 256>& evt_chan
)
    : m_stack(stack),
      m_req_q(req_q),
      m_req_chan(req_chan),
      m_evt_chan(evt_chan) {
}

int NetdCoreIpcBridge::req_notify_fd() const {
    return m_req_chan.notify_fd();
}

void NetdCoreIpcBridge::drain_req_notify() {
    m_req_chan.drain_notify();
}

void NetdCoreIpcBridge::drain_requests(uint32_t now_ms) {
    for (;;) {
        CoreReqMsg req{};
        if (!m_req_q.pop(req)) {
            break;
        }

        if (req.type == CoreReqType::PingSubmit) {
            const PingSubmitMsg& msg = req.ping;

            Ipv4Icmp::PingRequest pr{};
            pr.dst_ip_be = msg.dst_ip_be;
            pr.ident_be = msg.ident_be;
            pr.seq_be = msg.seq_be;
            pr.timeout_ms = msg.timeout_ms;
            pr.tag = msg.tag;
            pr.client_token = msg.client_token;

            (void)m_stack.submit_ping(pr, now_ms);
            continue;
        }

        if (req.type == CoreReqType::DnsResolveSubmit) {
            const DnsResolveSubmitMsg& msg = req.dns;

            ResolveRequest dr{};
            dr.name_len = msg.name_len;
            memcpy(dr.name, msg.name, msg.name_len);
            dr.tag = msg.tag;
            dr.client_token = msg.client_token;
            dr.timeout_ms = msg.timeout_ms;

            (void)m_stack.submit_resolve(dr, now_ms);
            continue;
        }
    }
}

void NetdCoreIpcBridge::publish_events(uint32_t now_ms) {
    (void)now_ms;

    for (;;) {
        Ipv4Icmp::PingResult r{};
        if (!m_stack.poll_ping_result(r)) {
            break;
        }

        CoreEvtMsg evt{};
        evt.type = CoreEvtType::PingResult;
        evt.ping.dst_ip_be = r.dst_ip_be;
        evt.ping.ident_be = r.ident_be;
        evt.ping.seq_be = r.seq_be;
        evt.ping.rtt_ms = r.rtt_ms;
        evt.ping.ok = r.ok;
        evt.ping.tag = r.tag;
        evt.ping.client_token = r.client_token;

        (void)m_evt_chan.push_and_wake(evt);
    }

    for (;;) {
        ResolveResult r{};
        if (!m_stack.poll_resolve_result(r)) {
            break;
        }

        CoreEvtMsg evt{};
        evt.type = CoreEvtType::DnsResolveResult;
        evt.dns.ip_be = r.ip_be;
        evt.dns.ok = r.ok;
        evt.dns.tag = r.tag;
        evt.dns.client_token = r.client_token;

        (void)m_evt_chan.push_and_wake(evt);
    }
}

}

#include "netd_app.h"

namespace {

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

namespace netd {

void NetdApp::poll_once(uint32_t now_ms) {
    int timeout_ms = 10;

    {
        uint32_t next_wakeup_ms = 0u;
        if (m_stack->try_get_next_wakeup_ms(now_ms, next_wakeup_ms)) {
        }

        if (next_wakeup_ms != 0u) {
            if (next_wakeup_ms <= now_ms) {
                timeout_ms = 0;
            } else {
                const uint32_t dt = next_wakeup_ms - now_ms;

                const uint32_t cap = 10u;
                if (dt < cap) {
                    timeout_ms = (int)dt;
                }
            }
        }
    }

    pollfd_t fds[2];

    fds[0].fd = m_dev.fd();
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    fds[1].fd = m_ipc_to_core_chan.notify_fd();
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    (void)poll(fds, 2, timeout_ms);

    if ((fds[1].revents & POLLIN) != 0) {
        m_ipc_to_core_chan.drain_notify();
    }
}

void NetdApp::drain_core_requests(uint32_t now_ms) {
    for (;;) {
        CoreReqMsg req{};
        if (!m_ipc_to_core_q.pop(req)) {
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

            (void)m_stack->submit_ping(pr, now_ms);
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

            (void)m_stack->submit_resolve(dr, now_ms);
            continue;
        }
    }
}

void NetdApp::publish_events(uint32_t now_ms) {
    for (;;) {
        Ipv4Icmp::PingResult r{};
        if (!m_stack->poll_ping_result(r)) {
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

        (void)m_core_to_ipc_chan.push_and_wake(evt);
    }

    for (;;) {
        ResolveResult r{};
        if (!m_stack->poll_resolve_result(r)) {
            break;
        }

        CoreEvtMsg evt{};
        evt.type = CoreEvtType::DnsResolveResult;
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
      m_stack(),
      m_ipc(),
      m_ipc_thread{},
      m_ipc_ctx{} {
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

    if (!init_stack()) {
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

bool NetdApp::init_stack() {
    NetdCoreStack* stack = m_stack.construct(m_core_arena, m_dev);
    if (!stack->init()) {
        printf("networkd: stack init failed\n");
        return false;
    }

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

    IpcServer* ipc = m_ipc.construct(m_ipc_arena, m_ipc_to_core_chan, m_core_to_ipc_q);
    if (!ipc->listen()) {
        printf("networkd: ipc_listen failed\n");
        return false;
    }

    m_ipc_ctx.ipc = ipc;
    m_ipc_ctx.notify = &m_core_to_ipc_notify;

    if (pthread_create(&m_ipc_thread, nullptr, &NetdApp::ipc_thread_main, &m_ipc_ctx) != 0) {
        printf("networkd: pthread_create failed\n");
        return false;
    }

    return true;
}

int NetdApp::run() {
    const Mac mac = m_stack->mac();

    printf("networkd: iface ne2k0 up\n");
    printf("networkd: mac ");
    print_mac(mac);
    printf("\n");

    printf("networkd: ip 10.0.2.15 mask 255.255.255.0 gw 10.0.2.2\n");

    {
        Mac gw_mac{};
        (void)m_stack->resolve_gateway(m_stack->default_gw_be(), gw_mac, 2000u);
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

            (void)m_stack->handle_frame(frame, flen, now);
        }

        drain_core_requests(now);

        m_stack->step(now);

        publish_events(now);
    }

    return 0;
}

}

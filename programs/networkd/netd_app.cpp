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

static void print_ipv4_be(uint32_t ip_be) {
    const uint32_t ip = netd::ntohl(ip_be);

    const uint8_t a = (uint8_t)((ip >> 24) & 0xFFu);
    const uint8_t b = (uint8_t)((ip >> 16) & 0xFFu);
    const uint8_t c = (uint8_t)((ip >> 8) & 0xFFu);
    const uint8_t d = (uint8_t)(ip & 0xFFu);

    printf("%u.%u.%u.%u", (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d);
}

}

namespace netd {

NetdApp::GatewayArpResolver::GatewayArpResolver()
    : m_stack(nullptr),
      m_sched(nullptr),
      m_gw_ip_be(0u),
      m_deadline_ms(0u),
      m_retry_ms(200u),
      m_active(false),
      m_done(false),
      m_ok(false),
      m_timer(),
      m_mac{} {
}

void NetdApp::GatewayArpResolver::start(
    NetdCoreStack& stack,
    NetdTickScheduler& sched,
    uint32_t gw_ip_be,
    uint32_t timeout_ms,
    uint32_t now_ms
) {
    stop();

    m_stack = &stack;
    m_sched = &sched;
    m_gw_ip_be = gw_ip_be;
    m_deadline_ms = now_ms + timeout_ms;
    m_retry_ms = 200u;
    m_active = true;
    m_done = false;
    m_ok = false;
    m_mac = Mac{};

    schedule_next(now_ms, 0u);
}

void NetdApp::GatewayArpResolver::stop() {
    if (m_sched && m_timer.is_valid()) {
        (void)m_sched->cancel(m_timer);
    }

    m_timer = TimerId();
    m_active = false;
}

bool NetdApp::GatewayArpResolver::is_active() const {
    return m_active;
}

bool NetdApp::GatewayArpResolver::is_done() const {
    return m_done;
}

bool NetdApp::GatewayArpResolver::ok() const {
    return m_ok;
}

const Mac& NetdApp::GatewayArpResolver::mac() const {
    return m_mac;
}

void NetdApp::GatewayArpResolver::on_timer(void* ctx, uint32_t now_ms) {
    GatewayArpResolver* self = (GatewayArpResolver*)ctx;
    if (!self) {
        return;
    }

    self->handle_timer(now_ms);
}

void NetdApp::GatewayArpResolver::handle_timer(uint32_t now_ms) {
    if (!m_active || !m_stack || !m_sched) {
        return;
    }

    Mac mac{};
    if (m_stack->lookup_arp(m_gw_ip_be, mac, now_ms)) {
        m_mac = mac;
        m_ok = true;
        m_done = true;
        m_active = false;
        return;
    }

    if (now_ms >= m_deadline_ms) {
        m_ok = false;
        m_done = true;
        m_active = false;
        return;
    }

    (void)m_stack->request_arp(m_gw_ip_be);
    schedule_next(now_ms, m_retry_ms);
}

void NetdApp::GatewayArpResolver::schedule_next(uint32_t now_ms, uint32_t delay_ms) {
    if (!m_sched) {
        return;
    }

    m_timer = m_sched->schedule(delay_ms, this, &GatewayArpResolver::on_timer, now_ms);
}

void NetdApp::poll_once(uint32_t now_ms) {
    uint32_t next_wakeup_ms = 0u;
    (void)m_stack->try_get_next_wakeup_ms(now_ms, next_wakeup_ms);

    const int timeout_ms = m_sched.compute_poll_timeout_ms(now_ms, next_wakeup_ms);

    pollfd_t fds[2];

    fds[0].fd = m_dev.fd();
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    fds[1].fd = m_bridge->req_notify_fd();
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    (void)poll(fds, 2, timeout_ms);

    if ((fds[1].revents & POLLIN) != 0) {
        m_bridge->drain_req_notify();
    }
}

void NetdApp::drain_core_requests(uint32_t now_ms) {
    m_bridge->drain_requests(now_ms);
}

void NetdApp::publish_events(uint32_t now_ms) {
    m_bridge->publish_events(now_ms);
}

NetdApp::NetdApp()
    : m_core_arena(),
      m_ipc_arena(),
      m_cfg(default_netd_config()),
      m_dev(),
      m_ipc_to_core_q(),
      m_core_to_ipc_q(),
      m_core_to_ipc_notify(),
      m_ipc_to_core_notify(),
      m_ipc_to_core_chan(m_ipc_to_core_q, m_ipc_to_core_notify),
      m_core_to_ipc_chan(m_core_to_ipc_q, m_core_to_ipc_notify),
      m_stack(),
      m_bridge(),
      m_ipc(),
      m_sched(m_core_arena, 10u),
      m_ipc_rt(),
      m_gw_resolver() {
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

    if (!init_bridge()) {
        return false;
    }

    if (!init_ipc()) {
        return false;
    }

    if (!init_scheduler(uptime_ms())) {
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
    if (!stack->init(m_cfg)) {
        printf("networkd: stack init failed\n");
        return false;
    }

    return true;
}

bool NetdApp::init_bridge() {
    (void)m_bridge.construct(*m_stack.get(), m_ipc_to_core_q, m_ipc_to_core_chan, m_core_to_ipc_chan);
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

    if (!m_ipc_rt.start(*ipc, m_core_to_ipc_notify)) {
        printf("networkd: pthread_create failed\n");
        return false;
    }

    return true;
}

bool NetdApp::init_scheduler(uint32_t now_ms) {
    if (!m_sched.init(now_ms)) {
        printf("networkd: scheduler init failed\n");
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

    printf("networkd: ip ");
    print_ipv4_be(m_cfg.ip_be);
    printf(" mask ");
    print_ipv4_be(m_cfg.mask_be);
    printf(" gw ");
    print_ipv4_be(m_cfg.gw_be);
    printf("\n");

    m_gw_resolver.start(*m_stack.get(), m_sched, m_cfg.gw_be, 2000u, uptime_ms());

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

        m_sched.tick(now);

        m_stack->step(now);

        publish_events(now);
    }

    return 0;
}

}

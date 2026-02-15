#include "ipc_server.h"

#include <yula.h>

namespace netd {

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

void IpcServer::send_queue_full_error(IpcServer& self, Client& c, uint32_t seq) {
    netd_ipc_error_t err{};
    err.code = -12;
    (void)self.send_msg(c, NETD_IPC_MSG_ERROR, seq, &err, (uint32_t)sizeof(err));
}

bool IpcServer::RxBuffer::read_from(int fd, bool& out_error) {
    out_error = false;

    if (len >= sizeof(data)) {
        out_error = true;
        return false;
    }

    const uint32_t free = (uint32_t)sizeof(data) - len;
    const int got = pipe_try_read(fd, data + len, free);
    if (got < 0) {
        out_error = true;
        return false;
    }

    if (got > 0) {
        len += (uint32_t)got;
        return true;
    }

    return false;
}

bool IpcServer::RxBuffer::try_peek(
    netd_ipc_hdr_t& out_hdr,
    const uint8_t*& out_payload,
    uint32_t& out_total,
    bool& out_invalid
) const {
    out_invalid = false;

    if (len < (uint32_t)sizeof(netd_ipc_hdr_t)) {
        return false;
    }

    netd_ipc_hdr_t hdr{};
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != NETD_IPC_MAGIC || hdr.version != NETD_IPC_VERSION) {
        out_invalid = true;
        return false;
    }

    if (hdr.len > NETD_IPC_MAX_PAYLOAD) {
        out_invalid = true;
        return false;
    }

    const uint32_t total = (uint32_t)sizeof(netd_ipc_hdr_t) + hdr.len;
    if (len < total) {
        return false;
    }

    out_hdr = hdr;
    out_payload = data + sizeof(netd_ipc_hdr_t);
    out_total = total;
    return true;
}

void IpcServer::RxBuffer::consume(uint32_t count) {
    if (count >= len) {
        len = 0u;
        return;
    }

    const uint32_t remain = len - count;
    memmove(data, data + count, remain);
    len = remain;
}

IpcServer::IpcServer(
    Arena& arena,
    SpscChannel<CoreReqMsg, 256>& to_core,
    SpscQueue<CoreEvtMsg, 256>& from_core
)
    : m_to_core(to_core),
      m_from_core(from_core),
      m_listen_fd(-1),
      m_clients(arena),
      m_token_to_index(arena),
      m_pollfds(arena),
      m_dispatch(arena),
      m_next_token(1u) {
    (void)m_dispatch.reserve(8u);
    (void)m_dispatch.add(NETD_IPC_MSG_PING_REQ, this, &IpcServer::handle_ping_req);
    (void)m_dispatch.add(NETD_IPC_MSG_RESOLVE_REQ, this, &IpcServer::handle_resolve_req);
}

IpcServer::Client* IpcServer::try_get_client_by_token(uint32_t token) {
    uint32_t idx = 0;
    if (!m_token_to_index.get(token, idx)) {
        return nullptr;
    }

    if (idx >= m_clients.size()) {
        return nullptr;
    }

    Client& c = m_clients[idx];
    if (c.token != token) {
        return nullptr;
    }

    return &c;
}

void IpcServer::on_client_added(uint32_t client_index) {
    if (client_index >= m_clients.size()) {
        return;
    }

    const uint32_t token = m_clients[client_index].token;
    (void)m_token_to_index.put(token, client_index);
}

void IpcServer::on_client_removed(uint32_t client_index, uint32_t removed_token, uint32_t moved_token) {
    if (removed_token != 0u) {
        (void)m_token_to_index.erase(removed_token);
    }

    if (moved_token == 0u) {
        return;
    }

    (void)m_token_to_index.put(moved_token, client_index);
}

bool IpcServer::handle_ping_req(
    void* handler_ctx,
    void* call_ctx,
    uint16_t type,
    uint32_t seq,
    const uint8_t* payload,
    uint32_t len,
    uint32_t now_ms
) {
    (void)type;
    (void)now_ms;

    IpcServer* self = (IpcServer*)handler_ctx;
    Client* c = (Client*)call_ctx;
    if (!self || !c || !payload) {
        return false;
    }

    if (len != sizeof(netd_ipc_ping_req_t)) {
        return false;
    }

    netd_ipc_ping_req_t req{};
    memcpy(&req, payload, sizeof(req));

    CoreReqMsg msg{};
    msg.type = CoreReqType::PingSubmit;
    msg.ping.dst_ip_be = req.dst_ip_be;
    msg.ping.ident_be = req.ident_be;
    msg.ping.seq_be = req.seq_be;
    msg.ping.timeout_ms = clamp_u32(req.timeout_ms, 1u, 10000u);
    msg.ping.tag = seq;
    msg.ping.client_token = c->token;

    if (self->m_to_core.push_and_wake(msg)) {
        return true;
    }

    send_queue_full_error(*self, *c, seq);
    return true;
}

bool IpcServer::handle_resolve_req(
    void* handler_ctx,
    void* call_ctx,
    uint16_t type,
    uint32_t seq,
    const uint8_t* payload,
    uint32_t len,
    uint32_t now_ms
) {
    (void)type;
    (void)now_ms;

    IpcServer* self = (IpcServer*)handler_ctx;
    Client* c = (Client*)call_ctx;
    if (!self || !c || !payload) {
        return false;
    }

    if (len != sizeof(netd_ipc_resolve_req_t)) {
        return false;
    }

    netd_ipc_resolve_req_t req{};
    memcpy(&req, payload, sizeof(req));

    if (req.name_len == 0u || req.name_len > sizeof(req.name)) {
        return false;
    }

    CoreReqMsg msg{};
    msg.type = CoreReqType::DnsResolveSubmit;
    msg.dns.name_len = req.name_len;
    memcpy(msg.dns.name, req.name, req.name_len);
    msg.dns.timeout_ms = clamp_u32(req.timeout_ms, 1u, 10000u);
    msg.dns.tag = seq;
    msg.dns.client_token = c->token;

    if (self->m_to_core.push_and_wake(msg)) {
        return true;
    }

    send_queue_full_error(*self, *c, seq);
    return true;
}

bool IpcServer::listen() {
    m_listen_fd = ipc_listen("networkd");
    return m_listen_fd >= 0;
}

int IpcServer::wait(const PipePair& notify, int timeout_ms) {
    if (m_listen_fd < 0) {
        return -1;
    }

    const int notify_fd = notify.read_fd();

    const uint32_t need = m_clients.size() + 2u;

    if (m_pollfds.capacity() < need) {
        if (!m_pollfds.reserve(need)) {
            return -1;
        }
    }

    m_pollfds.clear();

    pollfd_t pfd{};
    pfd.fd = m_listen_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    (void)m_pollfds.push_back(pfd);

    pfd.fd = notify_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    (void)m_pollfds.push_back(pfd);

    for (uint32_t i = 0; i < m_clients.size(); i++) {
        const Client& c = m_clients[i];

        pfd.fd = c.fd_r.get();
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)m_pollfds.push_back(pfd);
    }

    if (m_pollfds.size() != need) {
        return -1;
    }

    const int pr = poll(m_pollfds.data(), m_pollfds.size(), timeout_ms);

    if (notify_fd >= 0) {
        for (uint32_t i = 0; i < m_pollfds.size(); i++) {
            if (m_pollfds[i].fd != notify_fd) {
                continue;
            }

            if ((m_pollfds[i].revents & POLLIN) == 0) {
                break;
            }

            notify.drain();
            break;
        }
    }

    return pr;
}

bool IpcServer::accept_one() {
    if (m_listen_fd < 0) {
        return false;
    }

    int fds[2] = { -1, -1 };
    const int ar = ipc_accept(m_listen_fd, fds);
    if (ar != 1) {
        return false;
    }

    Client c{};
    c.fd_r.reset(fds[0]);
    c.fd_w.reset(fds[1]);
    c.token = m_next_token;
    c.seq_out = 1;
    c.rx.len = 0;

    m_next_token++;
    if (m_next_token == 0u) {
        m_next_token = 1u;
    }

    if (!m_clients.push_back(netd::move(c))) {
        return false;
    }

    on_client_added(m_clients.size() - 1u);

    return true;
}

bool IpcServer::send_msg(Client& c, uint16_t type, uint32_t seq, const void* payload, uint32_t len) {
    if (len > NETD_IPC_MAX_PAYLOAD) {
        return false;
    }

    netd_ipc_hdr_t hdr{};
    hdr.magic = NETD_IPC_MAGIC;
    hdr.version = NETD_IPC_VERSION;
    hdr.type = type;
    hdr.len = len;
    hdr.seq = seq;

    if (write(c.fd_w.get(), &hdr, (uint32_t)sizeof(hdr)) != (int)sizeof(hdr)) {
        return false;
    }

    if (len > 0) {
        if (write(c.fd_w.get(), payload, len) != (int)len) {
            return false;
        }
    }

    return true;
}

void IpcServer::drop_client(uint32_t idx) {
    if (idx >= m_clients.size()) {
        return;
    }

    const uint32_t last = m_clients.size() - 1u;
    const uint32_t removed_token = m_clients[idx].token;
    const uint32_t moved_token = (idx != last) ? m_clients[last].token : 0u;

    on_client_removed(idx, removed_token, moved_token);
    m_clients.erase_unordered(idx);
}

bool IpcServer::client_step(Client& c, uint32_t now_ms) {
    (void)now_ms;

    bool read_error = false;
    (void)c.rx.read_from(c.fd_r.get(), read_error);
    if (read_error) {
        return true;
    }

    for (;;) {
        netd_ipc_hdr_t hdr{};
        const uint8_t* payload = nullptr;
        uint32_t total = 0u;
        bool invalid = false;

        if (!c.rx.try_peek(hdr, payload, total, invalid)) {
            if (invalid) {
                return true;
            }
            break;
        }

        if (!m_dispatch.dispatch(hdr.type, &c, hdr.seq, payload, hdr.len, now_ms)) {
            netd_ipc_error_t err{};
            err.code = -1;
            (void)send_msg(c, NETD_IPC_MSG_ERROR, hdr.seq, &err, (uint32_t)sizeof(err));
        }

        c.rx.consume(total);
    }

    return false;
}

void IpcServer::step(uint32_t now_ms) {
    (void)now_ms;

    for (;;) {
        if (!accept_one()) {
            break;
        }
    }

    for (uint32_t i = 0; i < m_clients.size();) {
        Client& c = m_clients[i];

        const bool should_drop = client_step(c, now_ms);

        if (should_drop || c.rx.len >= sizeof(c.rx.data)) {
            drop_client(i);
            continue;
        }

        i++;
    }

    for (;;) {
        CoreEvtMsg evt{};
        if (!m_from_core.pop(evt)) {
            break;
        }

        if (evt.type == CoreEvtType::PingResult) {
            const PingResultMsg& res = evt.ping;

            netd_ipc_ping_rsp_t rsp{};
            rsp.dst_ip_be = res.dst_ip_be;
            rsp.ident_be = res.ident_be;
            rsp.seq_be = res.seq_be;
            rsp.rtt_ms = res.rtt_ms;
            rsp.ok = res.ok ? 1u : 0u;

            Client* c = try_get_client_by_token(res.client_token);
            if (!c) {
                continue;
            }

            (void)send_msg(*c, NETD_IPC_MSG_PING_RSP, res.tag, &rsp, (uint32_t)sizeof(rsp));
            continue;
        }

        if (evt.type == CoreEvtType::DnsResolveResult) {
            const DnsResolveResultMsg& res = evt.dns;

            netd_ipc_resolve_rsp_t rsp{};
            rsp.ip_be = res.ip_be;
            rsp.ok = res.ok ? 1u : 0u;

            Client* c = try_get_client_by_token(res.client_token);
            if (!c) {
                continue;
            }

            (void)send_msg(*c, NETD_IPC_MSG_RESOLVE_RSP, res.tag, &rsp, (uint32_t)sizeof(rsp));
            continue;
        }
    }
}

}

#include "ipc_server.h"

#include <yula.h>

namespace netd {

namespace {

static int read_into_buffer(int fd, uint8_t* buf, uint32_t cap, uint32_t& inout_len) {
    if (!buf || cap == 0) {
        return -1;
    }

    if (inout_len >= cap) {
        return -1;
    }

    const uint32_t free = cap - inout_len;
    const int got = pipe_try_read(fd, buf + inout_len, free);
    if (got < 0) {
        return -1;
    }

    if (got > 0) {
        inout_len += (uint32_t)got;
    }

    return got;
}

}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

IpcServer::IpcServer(
    Arena& arena,
    SpscQueue<PingSubmitMsg, 256>& to_core,
    SpscQueue<PingResultMsg, 256>& from_core
)
    : m_to_core(to_core),
      m_from_core(from_core),
      m_listen_fd(-1),
      m_clients(arena),
      m_pollfds(arena),
      m_fdw_to_index(arena) {
    (void)m_fdw_to_index.reserve(32u);
}

bool IpcServer::listen() {
    m_listen_fd = ipc_listen("networkd");
    return m_listen_fd >= 0;
}

int IpcServer::wait(int notify_fd, int timeout_ms) {
    if (m_listen_fd < 0) {
        return -1;
    }

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

        pfd.fd = c.fd_r;
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

            uint8_t buf[64];
            (void)pipe_try_read(notify_fd, buf, (uint32_t)sizeof(buf));
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
    c.fd_r = fds[0];
    c.fd_w = fds[1];
    c.seq_out = 1;
    c.rx_len = 0;

    if (!m_clients.push_back(c)) {
        close(c.fd_r);
        close(c.fd_w);
        return false;
    }

    const uint32_t idx = m_clients.size() - 1u;
    (void)m_fdw_to_index.put((uint32_t)c.fd_w, idx);

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

    if (write(c.fd_w, &hdr, (uint32_t)sizeof(hdr)) != (int)sizeof(hdr)) {
        return false;
    }

    if (len > 0) {
        if (write(c.fd_w, payload, len) != (int)len) {
            return false;
        }
    }

    return true;
}

bool IpcServer::try_parse_msg(Client& c, netd_ipc_hdr_t& out_hdr, const uint8_t*& out_payload) {
    if (c.rx_len < (uint32_t)sizeof(netd_ipc_hdr_t)) {
        return false;
    }

    netd_ipc_hdr_t hdr{};
    memcpy(&hdr, c.rx_buf, sizeof(hdr));

    if (hdr.magic != NETD_IPC_MAGIC || hdr.version != NETD_IPC_VERSION) {
        return false;
    }

    if (hdr.len > NETD_IPC_MAX_PAYLOAD) {
        return false;
    }

    const uint32_t total = (uint32_t)sizeof(netd_ipc_hdr_t) + hdr.len;
    if (c.rx_len < total) {
        return false;
    }

    out_hdr = hdr;
    out_payload = c.rx_buf + sizeof(netd_ipc_hdr_t);

    const uint32_t remain = c.rx_len - total;
    if (remain > 0) {
        memmove(c.rx_buf, c.rx_buf + total, remain);
    }

    c.rx_len = remain;
    return true;
}

void IpcServer::drop_client(uint32_t idx) {
    if (idx >= m_clients.size()) {
        return;
    }

    Client& c = m_clients[idx];

    (void)m_fdw_to_index.erase((uint32_t)c.fd_w);

    close(c.fd_r);
    close(c.fd_w);

    const uint32_t last = m_clients.size() - 1u;
    if (idx != last) {
        const Client moved = m_clients[last];
        m_clients[idx] = moved;

        (void)m_fdw_to_index.put((uint32_t)moved.fd_w, idx);
    }

    m_clients.erase_unordered(idx);
}

void IpcServer::client_step(Client& c, uint32_t now_ms) {
    (void)now_ms;

    const int got = read_into_buffer(c.fd_r, c.rx_buf, (uint32_t)sizeof(c.rx_buf), c.rx_len);
    if (got < 0) {
        c.rx_len = (uint32_t)sizeof(c.rx_buf);
        return;
    }

    for (;;) {
        netd_ipc_hdr_t hdr{};
        const uint8_t* payload = nullptr;

        if (!try_parse_msg(c, hdr, payload)) {
            break;
        }

        if (hdr.type == NETD_IPC_MSG_PING_REQ && hdr.len == sizeof(netd_ipc_ping_req_t)) {
            netd_ipc_ping_req_t req{};
            memcpy(&req, payload, sizeof(req));

            PingSubmitMsg msg{};
            msg.dst_ip_be = req.dst_ip_be;
            msg.ident_be = req.ident_be;
            msg.seq_be = req.seq_be;
            msg.timeout_ms = clamp_u32(req.timeout_ms, 1u, 10000u);
            msg.tag = hdr.seq;
            msg.client_fd_w = (uint32_t)c.fd_w;

            if (!m_to_core.push(msg)) {
                netd_ipc_error_t err{};
                err.code = -12;
                (void)send_msg(c, NETD_IPC_MSG_ERROR, hdr.seq, &err, (uint32_t)sizeof(err));
            }

            continue;
        }

        netd_ipc_error_t err{};
        err.code = -1;
        (void)send_msg(c, NETD_IPC_MSG_ERROR, hdr.seq, &err, (uint32_t)sizeof(err));
    }
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

        client_step(c, now_ms);

        if (c.rx_len >= sizeof(c.rx_buf)) {
            drop_client(i);
            continue;
        }

        i++;
    }

    for (;;) {
        PingResultMsg res{};
        if (!m_from_core.pop(res)) {
            break;
        }

        netd_ipc_ping_rsp_t rsp{};
        rsp.dst_ip_be = res.dst_ip_be;
        rsp.ident_be = res.ident_be;
        rsp.seq_be = res.seq_be;
        rsp.rtt_ms = res.rtt_ms;
        rsp.ok = res.ok ? 1u : 0u;

        uint32_t idx = 0;
        if (!m_fdw_to_index.get(res.client_fd_w, idx)) {
            continue;
        }

        if (idx >= m_clients.size()) {
            continue;
        }

        Client& c = m_clients[idx];
        (void)send_msg(c, NETD_IPC_MSG_PING_RSP, res.tag, &rsp, (uint32_t)sizeof(rsp));
    }
}

}

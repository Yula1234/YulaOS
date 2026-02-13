#ifndef YOS_NETD_IPC_SERVER_H
#define YOS_NETD_IPC_SERVER_H

#include "net_core.h"
#include "net_vec.h"
#include "net_u32_map.h"
#include "net_channel.h"
#include "netd_msgs.h"
#include "net_dispatch.h"

#include <yos/netd_ipc.h>

#include <yula.h>

namespace netd {

class IpcServer {
public:
    IpcServer(
        Arena& arena,
        SpscChannel<PingSubmitMsg, 256>& to_core,
        SpscQueue<PingResultMsg, 256>& from_core
    );

    bool listen();
    void step(uint32_t now_ms);

    int wait(const PipePair& notify, int timeout_ms);

    int listen_fd() const { return m_listen_fd; }

private:
    static bool handle_ping_req(void* handler_ctx, void* call_ctx, uint16_t type, uint32_t seq, const uint8_t* payload, uint32_t len, uint32_t now_ms);

    struct Client {
        UniqueFd fd_r;
        UniqueFd fd_w;
        uint32_t token;
        uint32_t seq_out;
        uint8_t rx_buf[512];
        uint32_t rx_len;
    };

    bool accept_one();
    void client_step(Client& c, uint32_t now_ms);
    void drop_client(uint32_t idx);

    bool try_parse_msg(Client& c, netd_ipc_hdr_t& out_hdr, const uint8_t*& out_payload);
    bool send_msg(Client& c, uint16_t type, uint32_t seq, const void* payload, uint32_t len);

    SpscChannel<PingSubmitMsg, 256>& m_to_core;
    SpscQueue<PingResultMsg, 256>& m_from_core;

    int m_listen_fd;
    Vector<Client> m_clients;

    Vector<pollfd_t> m_pollfds;

    IpcMsgDispatch m_dispatch;

    U32Map m_token_to_index;
    uint32_t m_next_token;
};

}

#endif

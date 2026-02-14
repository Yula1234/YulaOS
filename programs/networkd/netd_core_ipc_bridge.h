#ifndef YOS_NETD_CORE_IPC_BRIDGE_H
#define YOS_NETD_CORE_IPC_BRIDGE_H

#include "net_channel.h"
#include "netd_core_stack.h"
#include "netd_msgs.h"

#include <stdint.h>

namespace netd {

class NetdCoreIpcBridge {
public:
    NetdCoreIpcBridge(
        NetdCoreStack& stack,
        SpscQueue<CoreReqMsg, 256>& req_q,
        SpscChannel<CoreReqMsg, 256>& req_chan,
        SpscChannel<CoreEvtMsg, 256>& evt_chan
    );

    NetdCoreIpcBridge(const NetdCoreIpcBridge&) = delete;
    NetdCoreIpcBridge& operator=(const NetdCoreIpcBridge&) = delete;

    int req_notify_fd() const;
    void drain_req_notify();

    void drain_requests(uint32_t now_ms);
    void publish_events(uint32_t now_ms);

private:
    NetdCoreStack& m_stack;

    SpscQueue<CoreReqMsg, 256>& m_req_q;
    SpscChannel<CoreReqMsg, 256>& m_req_chan;
    SpscChannel<CoreEvtMsg, 256>& m_evt_chan;
};

}

#endif

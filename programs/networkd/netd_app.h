#ifndef YOS_NETD_APP_H
#define YOS_NETD_APP_H

#include "arena.h"
#include "ipc_server.h"
#include "net_channel.h"
#include "netd_core_ipc_bridge.h"
#include "netd_core_stack.h"
#include "netd_ipc_runtime.h"
#include "netd_tick_scheduler.h"
#include "net_spsc.h"
#include "netdev.h"
#include "netd_msgs.h"
#include "net_inplace.h"

#include <yula.h>

namespace netd {

class NetdApp {
public:
    NetdApp();

    NetdApp(const NetdApp&) = delete;
    NetdApp& operator=(const NetdApp&) = delete;

    bool init();
    int run();

private:
    bool init_arenas();
    bool init_device();
    bool init_stack();
    bool init_bridge();
    bool init_ipc();

    void poll_once(uint32_t now_ms);
    void drain_core_requests(uint32_t now_ms);
    void publish_events(uint32_t now_ms);

    Arena m_core_arena;
    Arena m_ipc_arena;

    NetDev m_dev;

    SpscQueue<CoreReqMsg, 256> m_ipc_to_core_q;
    SpscQueue<CoreEvtMsg, 256> m_core_to_ipc_q;

    PipePair m_core_to_ipc_notify;
    PipePair m_ipc_to_core_notify;

    SpscChannel<CoreReqMsg, 256> m_ipc_to_core_chan;
    SpscChannel<CoreEvtMsg, 256> m_core_to_ipc_chan;

    Inplace<NetdCoreStack> m_stack;
    Inplace<NetdCoreIpcBridge> m_bridge;
    Inplace<IpcServer> m_ipc;

    NetdIpcRuntime m_ipc_rt;
    NetdTickScheduler m_sched;
};

}

#endif

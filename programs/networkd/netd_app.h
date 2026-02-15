#ifndef YOS_NETD_APP_H
#define YOS_NETD_APP_H

#include "arena.h"
#include "ipc_server.h"
#include "net_channel.h"
#include "netd_config.h"
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
    struct GatewayArpResolver {
        GatewayArpResolver();

        void start(NetdCoreStack& stack, NetdTickScheduler& sched, uint32_t gw_ip_be, uint32_t timeout_ms, uint32_t now_ms);
        void stop();

        bool is_active() const;
        bool is_done() const;
        bool ok() const;
        const Mac& mac() const;

    private:
        static void on_timer(void* ctx, uint32_t now_ms);
        void handle_timer(uint32_t now_ms);
        void schedule_next(uint32_t now_ms, uint32_t delay_ms);
        void schedule_next_at(uint32_t now_ms, uint32_t next_ms);

        NetdCoreStack* m_stack;
        NetdTickScheduler* m_sched;
        uint32_t m_gw_ip_be;
        uint32_t m_deadline_ms;
        uint32_t m_retry_ms;
        bool m_active;
        bool m_done;
        bool m_ok;
        TimerId m_timer;
        ArpWaitState m_wait;
        Mac m_mac;
    };

    bool init_arenas();
    bool init_device();
    bool init_stack();
    bool init_bridge();
    bool init_ipc();
    bool init_scheduler(uint32_t now_ms);

    void poll_once(uint32_t now_ms);
    void read_frames(uint8_t* frame, uint32_t cap, uint32_t now_ms);
    void drain_core_requests(uint32_t now_ms);
    void tick_scheduler(uint32_t now_ms);
    void step_stack(uint32_t now_ms);
    void publish_events(uint32_t now_ms);

    Arena m_core_arena;
    Arena m_ipc_arena;

    NetdConfig m_cfg;

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

    NetdTickScheduler m_sched;
    NetdIpcRuntime m_ipc_rt;

    GatewayArpResolver m_gw_resolver;
};

}

#endif

#ifndef YOS_NETD_IPC_RUNTIME_H
#define YOS_NETD_IPC_RUNTIME_H

#include "ipc_server.h"

#include <yula.h>

namespace netd {

class NetdIpcRuntime {
public:
    NetdIpcRuntime();

    NetdIpcRuntime(const NetdIpcRuntime&) = delete;
    NetdIpcRuntime& operator=(const NetdIpcRuntime&) = delete;

    bool start(IpcServer& ipc, const PipePair& notify);

private:
    struct ThreadCtx {
        IpcServer* ipc;
        const PipePair* notify;
    };

    static void* thread_main(void* arg);

    pthread_t m_thread;
    ThreadCtx m_ctx;
};

}

#endif

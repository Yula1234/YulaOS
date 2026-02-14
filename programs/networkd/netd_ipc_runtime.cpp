#include "netd_ipc_runtime.h"

namespace netd {

NetdIpcRuntime::NetdIpcRuntime() : m_thread{}, m_ctx{} {
    m_ctx.ipc = nullptr;
    m_ctx.notify = nullptr;
}

void* NetdIpcRuntime::thread_main(void* arg) {
    ThreadCtx* ctx = (ThreadCtx*)arg;
    if (!ctx || !ctx->ipc || !ctx->notify) {
        return nullptr;
    }

    for (;;) {
        (void)ctx->ipc->wait(*ctx->notify, -1);

        const uint32_t now = uptime_ms();
        ctx->ipc->step(now);
    }
}

bool NetdIpcRuntime::start(IpcServer& ipc, const PipePair& notify) {
    m_ctx.ipc = &ipc;
    m_ctx.notify = &notify;

    if (pthread_create(&m_thread, nullptr, &NetdIpcRuntime::thread_main, &m_ctx) != 0) {
        m_ctx.ipc = nullptr;
        m_ctx.notify = nullptr;
        return false;
    }

    return true;
}

}

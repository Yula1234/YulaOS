// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/ipc_endpoint.h>

#include <fs/vfs.h>
#include <fs/pipe.h>

#include <kernel/proc.h>
#include <kernel/poll_waitq.h>

#include <hal/lock.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/intrusive_ref.h>
#include <lib/cpp/new.h>
#include <lib/cpp/utility.h>
#include <lib/cpp/vfs.h>
#include <lib/dlist.h>
#include <lib/string.h>
#include <lib/hash_map.h>

static bool ipc_name_valid(const char* name) {
    if (!name) {
        return false;
    }

    const size_t nlen = strlen(name);
    return nlen > 0 && nlen <= IPC_NAME_MAX;
}

class IpcEndpoint;

struct IpcPendingConn {
    dlist_head_t node;
    kernel::IntrusiveRef<IpcEndpoint> owner;
    uint32_t client_pid;
    kernel::VirtualFSNode c2s_r;
    kernel::VirtualFSNode s2c_w;
    spinlock_t lock;
    uint32_t refcount;
    uint32_t queued;

    IpcPendingConn(kernel::IntrusiveRef<IpcEndpoint>&& ep,
                   uint32_t pid,
                   kernel::VirtualFSNode&& in_r,
                   kernel::VirtualFSNode&& out_w) {
        dlist_init(&node);

        owner = kernel::move(ep);
        client_pid = pid;
        c2s_r = kernel::move(in_r);
        s2c_w = kernel::move(out_w);
        queued = 0u;
        refcount = 1u;

        spinlock_init(&lock);
    }

    IpcPendingConn(const IpcPendingConn&) = delete;
    IpcPendingConn& operator=(const IpcPendingConn&) = delete;
    IpcPendingConn(IpcPendingConn&&) = delete;
    IpcPendingConn& operator=(IpcPendingConn&&) = delete;

    void retain() {
        kernel::SpinLockSafeGuard guard(lock);
        refcount++;
    }

    void release();

    void discard_nodes() {
        c2s_r.reset();
        s2c_w.reset();
    }

    void mark_queued(bool value) {
        queued = value ? 1u : 0u;
    }

    bool is_queued() const {
        return queued != 0u;
    }
};

class IpcEndpoint {
public:
    IpcEndpoint(const char* n, vfs_node_t* node) {
        strlcpy(name, n, sizeof(name));
        spinlock_init(&lock);
        dlist_init(&pending_conns);
        poll_waitq_init(&poll_waitq);

        listen_node = node;
        refcount = 1u;
        closing = 0u;
    }

    IpcEndpoint(const IpcEndpoint&) = delete;
    IpcEndpoint& operator=(const IpcEndpoint&) = delete;
    IpcEndpoint(IpcEndpoint&&) = delete;
    IpcEndpoint& operator=(IpcEndpoint&&) = delete;

    const char* endpoint_name() const {
        return name;
    }

    bool retain() {
        kernel::SpinLockSafeGuard guard(lock);
        if (closing != 0u) {
            return false;
        }

        refcount++;
        return true;
    }

    void release() {
        bool do_finalize = false;

        {
            kernel::SpinLockSafeGuard guard(lock);
            if (refcount > 0u) {
                refcount--;
            }
            if (closing != 0u && refcount == 0u) {
                do_finalize = true;
            }
        }

        if (do_finalize) {
            finalize();
        }
    }

    void shutdown() {
        dlist_head_t to_release;
        dlist_init(&to_release);

        {
            kernel::SpinLockSafeGuard guard(lock);
            closing = 1u;
            while (!dlist_empty(&pending_conns)) {
                IpcPendingConn* p = container_of(pending_conns.next, IpcPendingConn, node);
                dlist_del(&p->node);
                p->mark_queued(false);
                dlist_add_tail(&p->node, &to_release);
            }
        }

        poll_waitq_wake_all(&poll_waitq);

        while (!dlist_empty(&to_release)) {
            IpcPendingConn* p = container_of(to_release.next, IpcPendingConn, node);
            dlist_del(&p->node);
            p->release();
        }
    }

    bool enqueue_pending(IpcPendingConn* conn) {
        {
            kernel::SpinLockSafeGuard guard(lock);
            if (closing != 0u) {
                return false;
            }

            conn->retain();
            conn->mark_queued(true);
            dlist_add_tail(&conn->node, &pending_conns);
        }

        poll_waitq_wake_all(&poll_waitq);
        return true;
    }

    bool remove_pending(IpcPendingConn* conn) {
        kernel::SpinLockSafeGuard guard(lock);
        if (!conn->is_queued()) {
            return false;
        }

        dlist_del(&conn->node);
        conn->mark_queued(false);
        return true;
    }

    bool has_pending() {
        kernel::SpinLockSafeGuard guard(lock);
        return !dlist_empty(&pending_conns);
    }

    int register_waiter(poll_waiter_t* w, task_t* task) {
        return poll_waitq_register(&poll_waitq, w, task);
    }

    IpcPendingConn* pop_pending() {
        kernel::SpinLockSafeGuard guard(lock);
        if (dlist_empty(&pending_conns)) {
            return nullptr;
        }

        IpcPendingConn* p = container_of(pending_conns.next, IpcPendingConn, node);
        dlist_del(&p->node);
        p->mark_queued(false);
        return p;
    }

private:
    void finalize() {
        poll_waitq_detach_all(&poll_waitq);
        delete this;
    }

    char name[IPC_NAME_MAX + 1u];
    spinlock_t lock;
    dlist_head_t pending_conns;
    poll_waitq_t poll_waitq;
    vfs_node_t* listen_node;
    uint32_t refcount;
    uint32_t closing;
};

void IpcPendingConn::release() {
    bool destroy = false;

    {
        kernel::SpinLockSafeGuard guard(lock);
        if (refcount > 0u) {
            refcount--;
        }
        destroy = (refcount == 0u);
    }

    if (!destroy) {
        return;
    }

    c2s_r.reset();
    s2c_w.reset();
    delete this;
}

struct IpcEndpointName {
    char data[IPC_NAME_MAX + 1u];

    IpcEndpointName() {
        data[0] = 0;
    }

    explicit IpcEndpointName(const char* s) {
        strlcpy(data, s ? s : "", sizeof(data));
    }

    bool operator==(const IpcEndpointName& other) const {
        return strcmp(data, other.data) == 0;
    }
};

static spinlock_t g_endpoints_lock;

class IpcEndpointRegistry {
public:
    bool add(const IpcEndpointName& name, IpcEndpoint* ep) {
        kernel::SpinLockSafeGuard guard(g_endpoints_lock);
        return endpoints.insert_unique(name, ep);
    }

    void remove(const IpcEndpointName& name) {
        kernel::SpinLockSafeGuard guard(g_endpoints_lock);
        endpoints.remove(name);
    }

    bool find_and_retain(const IpcEndpointName& name, kernel::IntrusiveRef<IpcEndpoint>& out) {
        kernel::SpinLockSafeGuard guard(g_endpoints_lock);

        return endpoints.with_value(name, [&out](IpcEndpoint* ep) -> bool {
            if (!ep) {
                return false;
            }

            out = kernel::IntrusiveRef<IpcEndpoint>::from_borrowed(ep);
            return (bool)out;
        });
    }

private:
    HashMap<IpcEndpointName, IpcEndpoint*, 128> endpoints;
};

template<>
uint32_t HashMap<IpcEndpointName, IpcEndpoint*, 128>::hash(const IpcEndpointName& key) {
    uint32_t h = 5381u;
    const char* s = key.data;
    while (*s) {
        h = ((h << 5) + h) + (uint32_t)(*s);
        s++;
    }
    return h;
}

static IpcEndpointRegistry g_endpoints;

extern "C" {

static int ipc_listen_close(vfs_node_t* node);

static vfs_ops_t ipc_listen_ops = {
    .read = 0,
    .write = 0,
    .open = 0,
    .close = ipc_listen_close,
    .ioctl = 0,
};

struct vfs_node* ipc_listen_create(const char* name) {
    vfs_node_t* node = nullptr;
    if (ipc_name_valid(name)) {
        node = new (kernel::nothrow) vfs_node_t;
    }
    if (!node) {
        return nullptr;
    }

    IpcEndpoint* ep = new (kernel::nothrow) IpcEndpoint(name, node);
    if (!ep) {
        delete node;
        return nullptr;
    }

    memset(node, 0, sizeof(*node));
    strlcpy(node->name, "ipc_listen", sizeof(node->name));
    node->flags = VFS_FLAG_IPC_LISTEN;
    node->refs = 1;
    node->ops = &ipc_listen_ops;
    node->private_data = ep;

    if (!g_endpoints.add(IpcEndpointName(name), ep)) {
        delete node;
        ep->shutdown();
        ep->release();
        return nullptr;
    }

    return node;
}

static int ipc_listen_close(vfs_node_t* node) {
    if (!node) {
        return -1;
    }

    IpcEndpoint* ep = (IpcEndpoint*)node->private_data;
    if (!ep) {
        delete node;
        return 0;
    }

    g_endpoints.remove(IpcEndpointName(ep->endpoint_name()));

    ep->shutdown();
    ep->release();
    delete node;
    return 0;
}

int ipc_connect(const char* name,
                struct vfs_node** out_c2s_w,
                struct vfs_node** out_s2c_r,
                void** out_pending_handle) {
    if (out_c2s_w) {
        *out_c2s_w = 0;
    }
    if (out_s2c_r) {
        *out_s2c_r = 0;
    }
    if (out_pending_handle) {
        *out_pending_handle = 0;
    }

    if (!out_c2s_w || !out_s2c_r || !out_pending_handle) {
        return -1;
    }
    if (!ipc_name_valid(name)) {
        return -1;
    }

    kernel::IntrusiveRef<IpcEndpoint> ep;
    if (!g_endpoints.find_and_retain(IpcEndpointName(name), ep)) {
        return -1;
    }

    IpcEndpoint* ep_raw = ep.get();

    kernel::VirtualFSPipe c2s = kernel::create_pipe();
    if (!c2s) {
        return -1;
    }

    kernel::VirtualFSPipe s2c = kernel::create_pipe();
    if (!s2c) {
        return -1;
    }

    task_t* curr = proc_current();
    IpcPendingConn* p = new (kernel::nothrow) IpcPendingConn(
        kernel::move(ep),
        curr ? curr->pid : 0u,
        kernel::move(c2s.read),
        kernel::move(s2c.write)
    );
    if (!p) {
        return -1;
    }

    if (!ep_raw->enqueue_pending(p)) {
        p->release();
        return -1;
    }

    *out_c2s_w = c2s.write.release();
    *out_s2c_r = s2c.read.release();
    *out_pending_handle = (void*)p;

    return 0;
}

void ipc_connect_commit(void* pending_handle) {
    IpcPendingConn* p = (IpcPendingConn*)pending_handle;
    if (!p) {
        return;
    }

    p->release();
}

void ipc_connect_cancel(void* pending_handle) {
    IpcPendingConn* p = (IpcPendingConn*)pending_handle;
    if (!p) {
        return;
    }

    IpcEndpoint* ep = p->owner.get();
    if (ep && ep->remove_pending(p)) {
        p->release();
    }
    p->release();
}

int ipc_listen_poll_ready(struct vfs_node* listen_node) {
    if (!listen_node) {
        return 0;
    }
    if ((listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) {
        return 0;
    }

    IpcEndpoint* ep = (IpcEndpoint*)listen_node->private_data;
    if (!ep) {
        return 0;
    }

    return ep->has_pending();
}

int ipc_listen_poll_waitq_register(struct vfs_node* listen_node, poll_waiter_t* w, task_t* task) {
    if (!listen_node || !w || !task) {
        return -1;
    }
    if ((listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) {
        return -1;
    }

    IpcEndpoint* ep = (IpcEndpoint*)listen_node->private_data;
    if (!ep) {
        return -1;
    }

    return ep->register_waiter(w, task);
}

int ipc_accept(struct vfs_node* listen_node,
               struct vfs_node** out_c2s_r,
               struct vfs_node** out_s2c_w) {
    if (out_c2s_r) {
        *out_c2s_r = 0;
    }
    if (out_s2c_w) {
        *out_s2c_w = 0;
    }

    if (!listen_node || !out_c2s_r || !out_s2c_w) {
        return -1;
    }
    if ((listen_node->flags & VFS_FLAG_IPC_LISTEN) == 0) {
        return -1;
    }

    IpcEndpoint* ep = (IpcEndpoint*)listen_node->private_data;
    if (!ep) {
        return -1;
    }

    for (;;) {
        IpcPendingConn* p = ep->pop_pending();
        if (!p) {
            return 0;
        }

        bool ok = true;
        if (p->client_pid != 0) {
            task_t* t = proc_find_by_pid(p->client_pid);
            if (!t || t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
                ok = false;
            }
        }

        if (!ok) {
            p->discard_nodes();
            p->release();
            continue;
        }

        *out_c2s_r = p->c2s_r.release();
        *out_s2c_w = p->s2c_w.release();
        p->release();

        return 1;
    }
}

}
